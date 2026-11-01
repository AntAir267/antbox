/*
 * android_host.c - see android_host.h.
 */
#if defined(__ANDROID__)

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "android_host.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

static char s_tag[32] = "xboxrecomp";
static int s_log_fd = -1;       /* the log file, or -1 */
static int s_pipe_rd = -1;

/* stdout and stderr end in a pipe; this thread turns what arrives into
 * logcat lines and appends the bytes to the log file as they came. A line
 * longer than the buffer is cut where the buffer ends. */
static void *log_pump(void *arg)
{
    char buf[1024];
    size_t have = 0;

    (void)arg;
    pthread_setname_np(pthread_self(), "stdio-logcat");
    for (;;) {
        ssize_t n = read(s_pipe_rd, buf + have, sizeof buf - 1 - have);
        size_t start = 0, i;

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        if (s_log_fd >= 0) {
            ssize_t w = write(s_log_fd, buf + have, (size_t)n);
            (void)w;
        }
        have += (size_t)n;
        for (i = 0; i < have; i++) {
            if (buf[i] != '\n')
                continue;
            buf[i] = '\0';
            if (i > start)
                __android_log_write(ANDROID_LOG_INFO, s_tag, buf + start);
            start = i + 1;
        }
        if (start == 0 && have == sizeof buf - 1) {     /* one long line */
            buf[have] = '\0';
            __android_log_write(ANDROID_LOG_INFO, s_tag, buf);
            have = 0;
        } else {
            memmove(buf, buf + start, have - start);
            have -= start;
        }
    }
    return NULL;
}

const char *recomp_android_dir(void)
{
    const char *d = getenv("RECOMP_ANDROID_DIR");
    return d && *d ? d : NULL;
}

const char *recomp_android_host_init(const char *tag, const char *logname)
{
    const char *dir = recomp_android_dir();
    int fds[2];
    pthread_t th;

    if (tag && *tag)
        snprintf(s_tag, sizeof s_tag, "%s", tag);
    if (dir && chdir(dir) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, s_tag, "chdir(%s): %s", dir, strerror(errno));
        dir = NULL;
    }
    if (dir && logname && *logname) {
        char path[1024], prev[1032];
        snprintf(path, sizeof path, "%s/%s", dir, logname);
        snprintf(prev, sizeof prev, "%s.1", path);
        rename(path, prev);
        s_log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    if (s_pipe_rd < 0 && pipe(fds) == 0) {
        s_pipe_rd = fds[0];
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        if (pthread_create(&th, NULL, log_pump, NULL) == 0)
            pthread_detach(th);
    }
    if (XR_DIAG_ON())       /* logcat follows the print gate (xr_log.h) */
        __android_log_print(ANDROID_LOG_INFO, s_tag, "host directory: %s", dir ? dir : "(none)");
    return dir;
}

int recomp_android_cpu_rank(int rank)
{
    cpu_set_t set;
    long freq[64], limit = 0x7fffffffL;
    int i, r, best = -1;

    if (sched_getaffinity(0, sizeof set, &set) != 0)
        return -1;
    for (i = 0; i < 64; i++) {
        char path[96];
        FILE *f;
        freq[i] = -1;
        if (i >= CPU_SETSIZE || !CPU_ISSET(i, &set))
            continue;
        snprintf(path, sizeof path,
                 "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fscanf(f, "%ld", &freq[i]) != 1)
            freq[i] = -1;
        fclose(f);
    }
    for (r = 0; r <= rank; r++) {
        long top = -1;
        best = -1;
        for (i = 0; i < 64; i++)
            if (freq[i] >= 0 && freq[i] < limit && freq[i] >= top) {
                top = freq[i];
                best = i;
            }
        if (best < 0)
            return -1;
        limit = top;
    }
    return best;
}

int recomp_android_fast_cpu(void)
{
    return recomp_android_cpu_rank(0);
}

#else
/* Not Android: nothing (ISO C forbids an empty translation unit). */
typedef int recomp_android_host_unused;
#endif

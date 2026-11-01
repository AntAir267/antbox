/*
 * recomp_instance.c - one running copy per save root. See recomp_instance.h.
 */
#include "recomp_instance.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>
#endif

#define LOCK_NAME "instance.lock"

#if defined(_WIN32)
static HANDLE s_lock = INVALID_HANDLE_VALUE;
#else
static int s_lock = -1;
#endif

static void nap(void)
{
#if defined(_WIN32)
    Sleep(100);
#else
    struct timespec ts = { 0, 100 * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

int recomp_instance_lock(const char *save_root, unsigned wait_ms)
{
    char path[1100];
    unsigned waited = 0;

    if (!save_root || !*save_root)
        return -1;
    snprintf(path, sizeof path, "%s/%s", save_root, LOCK_NAME);
#if defined(_WIN32)
    {
        WCHAR wpath[1100];
        if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1100))
            return -1;
        for (;;) {
            /* No sharing, not inherited (a restart's new process is started
             * while this one still runs): the open itself is the lock. */
            s_lock = CreateFileW(wpath, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, NULL);
            if (s_lock != INVALID_HANDLE_VALUE)
                break;
            if (GetLastError() != ERROR_SHARING_VIOLATION)
                return -1;
            if (waited >= wait_ms)
                return 0;
            nap();
            waited += 100;
        }
        {
            char line[64];
            DWORD n;
            snprintf(line, sizeof line, "%lu\n", (unsigned long)GetCurrentProcessId());
            SetEndOfFile(s_lock);
            WriteFile(s_lock, line, (DWORD)strlen(line), &n, NULL);
        }
    }
#else
    s_lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (s_lock < 0)
        return -1;
    for (;;) {
        if (flock(s_lock, LOCK_EX | LOCK_NB) == 0)
            break;
        if (errno != EWOULDBLOCK && errno != EINTR) {
            /* A file system without locks (some network and FUSE mounts):
             * run as before there was one. */
            close(s_lock);
            s_lock = -1;
            return -1;
        }
        if (waited >= wait_ms) {
            close(s_lock);
            s_lock = -1;
            return 0;
        }
        nap();
        waited += 100;
    }
    {
        char line[64];
        int n = snprintf(line, sizeof line, "%ld\n", (long)getpid());
        if (ftruncate(s_lock, 0) == 0 && write(s_lock, line, (size_t)n) < 0) {
            /* the pid is for a person looking; the lock is what counts */
        }
    }
#endif
    return 1;
}

void recomp_instance_unlock(void)
{
#if defined(_WIN32)
    if (s_lock != INVALID_HANDLE_VALUE)
        CloseHandle(s_lock);
    s_lock = INVALID_HANDLE_VALUE;
#else
    if (s_lock >= 0)
        close(s_lock);
    s_lock = -1;
#endif
}

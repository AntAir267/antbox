/* xbox_update_http_get on Linux and the other Unixes: the system's curl,
 * started directly (no shell). No library is linked or shipped for this;
 * SteamOS, Bazzite and the common desktops have curl. A system without it
 * answers "curl is not installed". */
#define _GNU_SOURCE
#include "xbox_update.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_BODY (1u << 20)

int xbox_update_available(void) { return 1; }

int xbox_update_http_get(const char *url, const char *user_agent, int timeout_ms,
                         char **body, char *err, size_t errn)
{
    int fd[2], wstatus = 0, status = 0;
    char secs[16];
    char *buf = NULL;
    size_t len = 0, cap = 0;
    pid_t pid;

    *body = NULL;
    err[0] = 0;
    snprintf(secs, sizeof secs, "%d", timeout_ms / 1000 > 0 ? timeout_ms / 1000 : 1);
    if (pipe(fd) != 0) {
        snprintf(err, errn, "cannot start curl");
        return 0;
    }
    pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        snprintf(err, errn, "cannot start curl");
        return 0;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        dup2(fd[1], 1);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 2);
        }
        close(fd[0]);
        close(fd[1]);
        /* An AppImage's own libraries are not for the system's curl. */
        if (getenv("APPDIR")) {
            unsetenv("LD_LIBRARY_PATH");
            unsetenv("LD_PRELOAD");
        }
        execlp("curl", "curl", "--silent", "--location", "--proto", "=https",
               "--connect-timeout", secs, "--max-time", secs,
               "--user-agent", user_agent, "--header", "Accept: application/vnd.github+json",
               "--write-out", "\n%{http_code}", "--", url, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    for (;;) {
        ssize_t got;
        if (len + 4096 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 8192;
            char *nb = (char *)realloc(buf, ncap);
            if (!nb)
                break;
            buf = nb;
            cap = ncap;
        }
        got = read(fd[0], buf + len, cap - len - 1);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        len += (size_t)got;
        if (len > MAX_BODY + 8)
            break;
    }
    close(fd[0]);
    if (len > MAX_BODY + 8)
        kill(pid, SIGKILL);
    while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR) {}

    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127) {
        snprintf(err, errn, "curl is not installed");
    } else if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0 && buf) {
        /* The body, a newline, the status code. */
        buf[len] = 0;
        char *nl = strrchr(buf, '\n');
        if (nl) {
            status = atoi(nl + 1);
            *nl = 0;
            *body = buf;
            buf = NULL;
        }
        if (!status)
            snprintf(err, errn, "no answer");
    } else if (WIFEXITED(wstatus)) {
        switch (WEXITSTATUS(wstatus)) {
        case 28:  snprintf(err, errn, "timed out"); break;
        case 5: case 6: case 7:
                  snprintf(err, errn, "no connection"); break;
        case 35: case 51: case 58: case 60:
                  snprintf(err, errn, "secure connection failed"); break;
        default:  snprintf(err, errn, "network error (curl %d)", WEXITSTATUS(wstatus));
        }
    } else {
        snprintf(err, errn, "network error");
    }
    free(buf);
    return status;
}

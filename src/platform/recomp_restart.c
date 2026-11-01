/*
 * recomp_restart.c - see recomp_restart.h.
 */
#include "recomp_restart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

static int    s_argc;
static char **s_argv;
static char **s_envp;
static char   s_cwd[2048];
static int    s_ready;
static volatile int s_requested;
static void (*s_hook)(void);
static int    s_out_fd = -1, s_err_fd = -1;  /* recomp_restart_stdio */
#if defined(_WIN32)
static wchar_t *s_wenv;                 /* the environment block as given */
static wchar_t  s_wcwd[2048];
#define RESTART_PARENT_W L"RECOMP_RESTART_PARENT"
static unsigned long s_waited_ms;       /* for the parent of a restart */
static int s_waited = -1;               /* -1 not a restart, 1 gone, 0 still there */
#endif

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static char **dup_vec(char **v, int n)
{
    char **d = (char **)calloc((size_t)n + 1, sizeof *d);
    int i;
    if (!d)
        return NULL;
    for (i = 0; i < n; i++)
        if (!(d[i] = dup_str(v[i] ? v[i] : "")))
            return NULL;
    return d;
}

#if !defined(_WIN32) && !defined(__ANDROID__)
/* The executable itself: argv[0] may be a bare name found on PATH. */
static const char *exe_path(void)
{
    static char path[PATH_MAX];
#if defined(__APPLE__)
    uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) == 0)
        return path;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n > 0) {
        path[n] = 0;
        return path;
    }
#endif
    return s_argv[0];
}
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
/* An AppImage (Linux): the program runs from the image's mount
 * (/tmp/.mount_XXX/usr/bin/...), which the image's runtime keeps only while
 * the process it started lives on unchanged. After an exec of the program
 * itself the mount is gone (the first restart works, the files it reads
 * later and a second restart do not: /proc/self/exe then names a path that
 * is not there). So a restart starts the image file again, in place: the
 * runtime mounts it afresh. Its arguments as the runtime got them (ARGV0
 * for the name), and the environment without what the runtime and AppRun
 * set for the old mount: APPIMAGE, APPDIR, ARGV0, OWD, and every entry of a
 * list (LD_LIBRARY_PATH, PATH, XDG_DATA_DIRS, ...) under the old APPDIR;
 * they set them again. */
static const char *s_exe;               /* NULL: the program itself */
static char **s_exec_argv, **s_exec_envp;

static int under(const char *path, const char *dir, size_t n)
{
    return n && !strncmp(path, dir, n) && (path[n] == '/' || path[n] == 0);
}

static char *drop_entries(const char *kv, const char *dir)
{
    const char *eq = strchr(kv, '='), *p;
    size_t n = strlen(dir), keep = 0;
    char *out;

    if (!eq || !strstr(eq + 1, dir))
        return dup_str(kv);
    out = (char *)malloc(strlen(kv) + 1);
    if (!out)
        return NULL;
    memcpy(out, kv, (size_t)(eq - kv) + 1);
    keep = (size_t)(eq - kv) + 1;
    for (p = eq + 1; ; ) {
        const char *end = strchr(p, ':');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        char item[4096];
        if (len < sizeof item) {
            memcpy(item, p, len);
            item[len] = 0;
            if (len && !under(item, dir, n)) {
                if (keep > (size_t)(eq - kv) + 1)
                    out[keep++] = ':';
                memcpy(out + keep, item, len);
                keep += len;
            }
        }
        if (!end)
            break;
        p = end + 1;
    }
    out[keep] = 0;
    if (keep == (size_t)(eq - kv) + 1) {    /* nothing left: the variable goes */
        free(out);
        return dup_str("");
    }
    return out;
}

static void plan_appimage(void)
{
    const char *img = getenv("APPIMAGE"), *dir = getenv("APPDIR"), *a0 = getenv("ARGV0");
    int i, n = 0;

    if (!img || !*img || access(img, X_OK) != 0)
        return;
    s_exec_argv = (char **)calloc((size_t)s_argc + 1, sizeof *s_exec_argv);
    while (s_envp[n])
        n++;
    s_exec_envp = (char **)calloc((size_t)n + 1, sizeof *s_exec_envp);
    if (!s_exec_argv || !s_exec_envp)
        return;
    s_exec_argv[0] = dup_str(a0 && *a0 ? a0 : img);
    for (i = 1; i < s_argc; i++)
        s_exec_argv[i] = s_argv[i];
    for (i = 0, n = 0; s_envp[i]; i++) {
        const char *kv = s_envp[i];
        char *v;
        if (!strncmp(kv, "APPIMAGE=", 9) || !strncmp(kv, "APPDIR=", 7) || !strncmp(kv, "ARGV0=", 6)
            || !strncmp(kv, "OWD=", 4))
            continue;
        v = dir && *dir ? drop_entries(kv, dir) : dup_str(kv);
        if (v && *v)
            s_exec_envp[n++] = v;
    }
    s_exe = dup_str(img);
    {
        /* A working folder inside the old mount is gone too: the one the
         * image was started from (OWD), as the runtime gives it. */
        const char *owd = getenv("OWD");
        if (dir && *dir && under(s_cwd, dir, strlen(dir)) && owd && *owd)
            snprintf(s_cwd, sizeof s_cwd, "%s", owd);
    }
}
#endif

/* The last atexit handler to run (the first registered). */
static void restart_at_exit(void)
{
    if (!s_requested || !s_ready)
        return;
    fflush(NULL);
#if defined(_WIN32)
    {
        /* No handle of this run goes along (as the exec's close-on-exec
         * elsewhere): an inherited log file kept the new run from moving the
         * last run's log aside, and nothing of the title's should outlive it.
         * The new process is told this one's id (RECOMP_RESTART_PARENT,
         * added to the environment as given and taken out again by
         * recomp_restart_init) and waits for it to be gone before it looks
         * at anything: Windows has no exec in place, and this process is
         * still there, with its files, until the last handler returns. */
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        wchar_t *cmd = _wcsdup(GetCommandLineW());
        wchar_t *env = NULL, var[64];
        size_t len = 0, vlen;

        swprintf(var, 64, L"%ls=%lu", RESTART_PARENT_W, (unsigned long)GetCurrentProcessId());
        vlen = wcslen(var) + 1;
        if (s_wenv) {
            const wchar_t *p = s_wenv;
            while (*p)
                p += wcslen(p) + 1;
            len = (size_t)(p - s_wenv);
        }
        env = (wchar_t *)malloc((len + vlen + 1) * sizeof *env);
        if (env) {
            if (len)
                memcpy(env, s_wenv, len * sizeof *env);
            memcpy(env + len, var, vlen * sizeof *env);
            env[len + vlen] = 0;
        }
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (cmd && env && CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_UNICODE_ENVIRONMENT,
                                         env, s_wcwd[0] ? s_wcwd : NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            fprintf(stderr, "[RESTART] the program could not be started again (error %lu)\n",
                    (unsigned long)GetLastError());
            fflush(stderr);
        }
        free(env);
        free(cmd);
    }
#elif !defined(__ANDROID__)
    {
        int fd, max = (int)sysconf(_SC_OPEN_MAX);
        const char *exe = exe_path();

        /* What this run opened (the log, devices, sockets) does not go
         * along: closed by the exec, not here, where other threads still
         * run. */
        if (max < 0 || max > 65536)
            max = 65536;
        for (fd = 3; fd < max; fd++) {
            int fl = fcntl(fd, F_GETFD);
            if (fl >= 0)
                fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
        }
        if (s_cwd[0] && chdir(s_cwd) != 0)
            fprintf(stderr, "[RESTART] cannot go back to %s\n", s_cwd);
        /* The streams' own destinations, not the log capture's pipes. */
        if (s_err_fd >= 0)
            dup2(s_err_fd, 2);
        if (s_out_fd >= 0)
            dup2(s_out_fd, 1);
        if (s_exe)
            exe = s_exe;                /* the AppImage itself (plan_appimage) */
        execve(exe, s_exe ? s_exec_argv : s_argv, s_exe ? s_exec_envp : s_envp);
        fprintf(stderr, "[RESTART] the program could not be started again (%s)\n", exe);
        fflush(stderr);
    }
#endif
}

void recomp_restart_init(int argc, char **argv)
{
    int n = 0;

    if (s_ready || argc < 1 || !argv)
        return;
    s_argc = argc;
    s_argv = dup_vec(argv, argc);
#if defined(_WIN32)
    {
        /* Started by a restart: wait for the process that started this one
         * to end (it is in its last exit handler), so two runs never hold
         * the save folder at once; then the variable goes, before the
         * environment is kept for the next restart. */
        wchar_t v[32];
        DWORD got = GetEnvironmentVariableW(RESTART_PARENT_W, v, 32);
        if (got > 0 && got < 32) {
            DWORD pid = (DWORD)wcstoul(v, NULL, 10), t0 = GetTickCount();
            HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
            s_waited = 1;
            if (h) {
                s_waited = WaitForSingleObject(h, 20000) == WAIT_OBJECT_0;
                CloseHandle(h);
            }
            s_waited_ms = GetTickCount() - t0;
            _wputenv_s(RESTART_PARENT_W, L"");     /* the CRT's copy and the process's */
            SetEnvironmentVariableW(RESTART_PARENT_W, NULL);
        }
    }
    {
        const wchar_t *env = GetEnvironmentStringsW(), *p = env;
        size_t len;
        if (env) {
            while (*p)
                p += wcslen(p) + 1;
            len = (size_t)(p - env) + 1;
            s_wenv = (wchar_t *)malloc(len * sizeof *s_wenv);
            if (s_wenv)
                memcpy(s_wenv, env, len * sizeof *s_wenv);
            FreeEnvironmentStringsW((LPWCH)env);
        }
        if (!GetCurrentDirectoryW(2048, s_wcwd))
            s_wcwd[0] = 0;
        if (!GetCurrentDirectoryA(sizeof s_cwd, s_cwd))
            s_cwd[0] = 0;
        s_envp = (char **)calloc(1, sizeof *s_envp);
        (void)n;
    }
#else
    while (environ && environ[n])
        n++;
    s_envp = dup_vec(environ, n);
    if (!getcwd(s_cwd, sizeof s_cwd))
        s_cwd[0] = 0;
#endif
    if (!s_argv || !s_envp)
        return;
#if !defined(_WIN32) && !defined(__ANDROID__)
    plan_appimage();
#endif
    s_ready = 1;
    atexit(restart_at_exit);
}

const char *recomp_restart_note(void)
{
#if defined(_WIN32)
    static char note[160];
    if (s_waited < 0)
        return "";
    snprintf(note, sizeof note, s_waited
             ? "[RESTART] started by a restart: the run before ended %lu ms after this one began\n"
             : "[RESTART] started by a restart: the run before was still there after %lu ms; going on\n",
             s_waited_ms);
    return note;
#else
    return "";
#endif
}

int recomp_restart_available(void)
{
    return s_ready || s_hook != NULL;
}

void recomp_restart_set_hook(void (*fn)(void))
{
    s_hook = fn;
}

void recomp_restart_stdio(int out_fd, int err_fd)
{
    s_out_fd = out_fd;
    s_err_fd = err_fd;
}

void recomp_restart_request(const char *why)
{
    fprintf(stderr, "[RESTART] asked for: %s\n", why && *why ? why : "(no reason given)");
    if (s_hook) {
        s_hook();
        return;
    }
    s_requested = 1;
}

int recomp_restart_requested(void)
{
    return s_requested;
}

void recomp_restart_cancel(void)
{
    s_requested = 0;
}

static void (*s_quit_hook)(void);

void recomp_quit_set_hook(void (*fn)(void))
{
    s_quit_hook = fn;
}

void recomp_quit_request(const char *why)
{
    fprintf(stderr, "[QUIT] asked for: %s\n", why && *why ? why : "(no reason given)");
    if (s_quit_hook)
        s_quit_hook();
}

void recomp_restart_target(const char **exe, char ***argv, char ***envp)
{
#if !defined(_WIN32) && !defined(__ANDROID__)
    if (exe) *exe = s_exe ? s_exe : exe_path();
    if (argv) *argv = s_exe ? s_exec_argv : s_argv;
    if (envp) *envp = s_exe ? s_exec_envp : s_envp;
#else
    if (exe) *exe = NULL;
    if (argv) *argv = s_argv;
    if (envp) *envp = s_envp;
#endif
}

void recomp_restart_snapshot(int *argc, char ***argv, char ***envp, const char **cwd)
{
    if (argc) *argc = s_argc;
    if (argv) *argv = s_argv;
    if (envp) *envp = s_envp;
    if (cwd)  *cwd = s_cwd;
}

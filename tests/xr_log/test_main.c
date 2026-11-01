/*
 * The print gate (xr_log.h, BO-125), as a release build starts:
 *
 *   quiet      xr_log_init(release_quiet = 1): every print to stdout or
 *              stderr is dropped, XR_ERROR / XR_NOTICE / XR_OUT are not;
 *              a dropped call evaluates none of its arguments
 *   files      a print to any other FILE * is written whatever the gate
 *   switches   the setting turns it on and off; RECOMP_LOG_DIAG,
 *              RECOMP_TRACE, RECOMP_DEBUG, RECOMP_STDIO_LOG,
 *              RECOMP_FLIP_MARK, RECOMP_HEADLESS and --headless force it,
 *              and then the setting does not move it; a developer build
 *              prints everything
 *
 * stdout and stderr go to temporary files (dup2) while each case runs and
 * are read back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define setenv(k, v, o) _putenv_s((k), (v))
#define unsetenv(k) _putenv_s((k), "")
#else
#include <unistd.h>
#endif

static int failures;
static char out_path[256], err_path[256];
static int saved_out = -1, saved_err = -1;

static void check(int ok, const char *what)
{
    (fprintf)(ok ? stdout : stderr, "  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static void capture_begin(void)
{
    fflush(stdout);
    fflush(stderr);
    saved_out = dup(fileno(stdout));
    saved_err = dup(fileno(stderr));
    (void)!freopen(out_path, "w", stdout);
    (void)!freopen(err_path, "w", stderr);
}

static size_t slurp(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    size_t k = 0;
    if (f) {
        k = fread(buf, 1, n - 1, f);
        fclose(f);
    }
    buf[k] = 0;
    return k;
}

static void capture_end(char *out, char *err, size_t n)
{
    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    slurp(out_path, out, n);
    slurp(err_path, err, n);
}

static int evaluated;
static int side(void) { evaluated++; return 7; }

static void prints(void)
{
    fprintf(stderr, "[DIAG] stderr %d\n", side());
    fprintf(stdout, "[DIAG] stdout\n");
    printf("[DIAG] printf %d\n", side());
    puts("[DIAG] puts");
    fputs("[DIAG] fputs\n", stderr);
    fputc('#', stderr);
    fwrite("[DIAG] fwrite\n", 1, 14, stderr);
    XR_ERROR("[ERR] error %d\n", 1);
    XR_NOTICE("[NOTE] notice\n");
    XR_OUT("RESULT OK\n");
}

static const char *const k_env[] = {
    "RECOMP_LOG_DIAG", "RECOMP_TRACE", "RECOMP_DEBUG", "RECOMP_STDIO_LOG",
    "RECOMP_FLIP_MARK", "RECOMP_HEADLESS", NULL
};

static void env_clear(void)
{
    int i;
    for (i = 0; k_env[i]; i++)
        unsetenv(k_env[i]);
}

int main(void)
{
    static char out[8192], err[8192];
    const char *tmp = getenv("TMPDIR");
    char file_path[256];
    FILE *f;
    int i;

    snprintf(out_path, sizeof out_path, "%s/xr_log_out.txt", tmp && *tmp ? tmp : ".");
    snprintf(err_path, sizeof err_path, "%s/xr_log_err.txt", tmp && *tmp ? tmp : ".");
    snprintf(file_path, sizeof file_path, "%s/xr_log_file.txt", tmp && *tmp ? tmp : ".");

    (fprintf)(stdout, "quiet (a release build, nothing set)\n");
    env_clear();
    xr_log_init(1, 0);
    evaluated = 0;
    capture_begin();
    prints();
    f = fopen(file_path, "w");
    if (f) {
        fprintf(f, "a file %d\n", 42);
        fputs("and more\n", f);
        fclose(f);
    }
    capture_end(out, err, sizeof out);
    check(!strstr(err, "[DIAG]") && !strstr(out, "[DIAG]") && !strchr(err, '#'),
          "no diagnostic line on stdout or stderr");
    check(strstr(err, "[ERR] error 1") && strstr(err, "[NOTE] notice"), "errors and notices are printed");
    check(strstr(out, "RESULT OK") != NULL, "program output (XR_OUT) is printed");
    check(evaluated == 0, "a dropped call evaluates none of its arguments");
    slurp(file_path, out, sizeof out);
    check(!strcmp(out, "a file 42\nand more\n"), "a print to a file is written");
    check(!xr_log_forced() && !strcmp(xr_log_source(), "build default"), "decided by the build");

    (fprintf)(stdout, "the setting\n");
    check(xr_log_set_diag(1) == 1, "on");
    evaluated = 0;
    capture_begin();
    prints();
    capture_end(out, err, sizeof out);
    check(strstr(err, "[DIAG] stderr 7") && strstr(out, "[DIAG] printf 7") && strstr(out, "[DIAG] puts")
          && strstr(err, "[DIAG] fwrite") && evaluated == 2, "everything printed");
    check(xr_log_set_diag(0) == 0 && !strcmp(xr_log_source(), "setting"), "off again");

    (fprintf)(stdout, "what forces it\n");
    for (i = 0; k_env[i]; i++) {
        char what[96];
        env_clear();
        setenv(k_env[i], "1", 1);
        xr_log_init(1, 0);
        snprintf(what, sizeof what, "%s=1 forces it on, the setting cannot undo it", k_env[i]);
        check(g_xr_log_diag == 1 && xr_log_forced() && xr_log_set_diag(0) == 1, what);
    }
    env_clear();
    setenv("RECOMP_LOG_DIAG", "0", 1);
    setenv("RECOMP_TRACE", "pacing", 1);
    xr_log_init(0, 1);
    check(g_xr_log_diag == 0 && xr_log_forced(), "RECOMP_LOG_DIAG=0 wins over a developer switch");
    /* The saved setting at start (BO-193): applied after init, it decides
     * when nothing in the environment does, and the environment still wins. */
    env_clear();
    xr_log_init(1, 0);
    check(g_xr_log_diag == 0 && !strcmp(xr_log_source(), "build default"), "release, before the file: off");
    check(xr_log_set_diag(1) == 1 && !xr_log_forced() && !strcmp(xr_log_source(), "setting"),
          "a saved diagnostic_log = true turns it on, source setting");
    capture_begin();
    xr_log_announce();
    capture_end(out, err, sizeof out);
    check(strstr(err, "diagnostic logging is on (setting)") != NULL, "the start-up line says on (setting)");
    xr_log_init(1, 0);
    xr_log_set_diag(0);
    capture_begin();
    xr_log_announce();
    capture_end(out, err, sizeof out);
    check(strstr(err, "diagnostic logging is off") && strstr(err, "(setting)"), "saved false: off (setting)");
    xr_log_init(0, 0);
    check(xr_log_set_diag(0) == 0, "a developer build with a saved false is quiet");
    setenv("RECOMP_LOG_DIAG", "0", 1);
    xr_log_init(1, 0);
    check(xr_log_set_diag(1) == 0 && xr_log_forced() && !strcmp(xr_log_source(), "RECOMP_LOG_DIAG"),
          "RECOMP_LOG_DIAG=0 beats a saved true");
    setenv("RECOMP_LOG_DIAG", "1", 1);
    xr_log_init(1, 0);
    check(xr_log_set_diag(0) == 1 && !strcmp(xr_log_source(), "RECOMP_LOG_DIAG"),
          "RECOMP_LOG_DIAG=1 beats a saved false");
    setenv("RECOMP_LOG_DIAG", "1", 1);
    xr_log_init(1, 0);
    capture_begin();
    xr_log_announce();
    capture_end(out, err, sizeof out);
    check(strstr(err, "diagnostic logging is on (RECOMP_LOG_DIAG)") != NULL, "the line names the variable");
    env_clear();
    xr_log_init(1, 1);
    check(g_xr_log_diag == 1 && !strcmp(xr_log_source(), "--headless"), "--headless forces it on");
    env_clear();
    setenv("RECOMP_HEADLESS", "0", 1);
    xr_log_init(1, 0);
    check(g_xr_log_diag == 0, "RECOMP_HEADLESS=0 does not");
    env_clear();
    xr_log_init(0, 0);
    check(g_xr_log_diag == 1 && xr_log_default() == 1, "a developer build prints everything");

    (fprintf)(stdout, failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}

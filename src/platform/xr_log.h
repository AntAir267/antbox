/*
 * xr_log.h - one gate for everything the runtime prints (BO-125).
 *
 * The runtime and the ports print with bare fprintf(stderr, ...) at about a
 * thousand call sites; a five-minute session is some 30 000 lines. A release
 * build prints none of that unless the player switches diagnostic logging
 * on. This header is force-included into every source of the toolkit and of
 * a port (the toolkit's CMakeLists: -include / /FI), so the existing call
 * sites go through the gate as they are.
 *
 * Three levels:
 *
 *   error       XR_ERROR(...)    always printed (stderr)
 *   notice      XR_NOTICE(...)   always printed (stderr): the few lines a
 *                                player or a bug report needs: the version,
 *                                the device and display, where the settings
 *                                and saves are
 *   diagnostic  every other print to stdout or stderr: printed only while
 *               g_xr_log_diag is set
 *
 * and one mark for a program's own output, which is not logging:
 *
 *   XR_OUT(...)                  always printed (stdout): the RESULT / STEP /
 *                                LOG lines of an installer run, a --help text
 *
 * How the gate tells a log line from a file being written: by the FILE *.
 * fprintf, fputs, fputc, fwrite and vfprintf are wrapped; the wrapper drops
 * the call only when diagnostics are off AND the stream is stdout or stderr.
 * A print to any other FILE * (a WAV, a trace CSV, a report) calls the real
 * function exactly as before. printf and puts have no stream argument and
 * are stdout by definition. Nothing is decided from the text.
 *
 * A dropped call evaluates none of its arguments and formats nothing: the
 * wrapper is `drop ? 0 : real(args)`, so the cost of a quiet line is one
 * load and a compare. (For the same reason an argument with a side effect
 * must not sit in a diagnostic print; there was none when this was written.)
 * The stream argument is evaluated twice when the call goes through.
 *
 * The switch: xr_log_init() at the start of main, then xr_log_set_diag()
 * from the settings (advanced.diagnostic_log), which xbox_SettingsInit does
 * as soon as it has read the file, and again on every change in the overlay.
 * xr_log_announce() says the result once, after that. Precedence, first that says
 * anything: RECOMP_LOG_DIAG=0|1; a developer switch that implies wanting the
 * log (RECOMP_TRACE, RECOMP_DEBUG, RECOMP_STDIO_LOG, RECOMP_FLIP_MARK,
 * RECOMP_HEADLESS or --headless); the setting; the build's default (quiet
 * for a release build, as today for every other). A program that never
 * calls xr_log_init prints everything, as before: the unit tests and tools.
 */
#ifndef XR_LOG_H
#define XR_LOG_H

#if !defined(__ASSEMBLER__) && !defined(RC_INVOKED)

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One definition for the program however many sources include this, and
 * no library needed to have it (a test that compiles one file links). */
#if defined(_WIN32)
#define XR_LOG_SHARED __declspec(selectany)
#else
#define XR_LOG_SHARED __attribute__((weak))
#endif
XR_LOG_SHARED int g_xr_log_diag = 1;

static inline int xr_log_drop_(FILE *f)
{
    return !g_xr_log_diag && (f == stderr || f == stdout);
}

/* src/platform/xr_log.c (the recomp_env library). */
/* release_quiet: this build is quiet unless told otherwise. headless: the
 * program was started with --headless. Reads the environment itself. */
void xr_log_init(int release_quiet, int headless);
/* The setting (advanced.diagnostic_log). Ignored while the environment
 * decides (xr_log_forced). Returns the state in force. */
int  xr_log_set_diag(int on);
/* What a run with nothing said does: 1 a developer build, 0 a release. */
int  xr_log_default(void);
/* 1 when the environment decided (the overlay shows the row locked). */
int  xr_log_forced(void);
/* What decided, in words, for the report: "RECOMP_LOG_DIAG", "RECOMP_TRACE",
 * "--headless", "setting", "build default". */
const char *xr_log_source(void);
/* The one start-up line (XR_NOTICE) with the state in force and what decided
 * it. Call it after the settings are read (xbox_SettingsInit), not before:
 * until then the setting has not been applied (BO-193). */
void xr_log_announce(void);

#ifdef __cplusplus
}
#endif

/* The marks: through the real functions, whatever the gate says. */
#define XR_ERROR(...)   ((void)(fprintf)(stderr, __VA_ARGS__))
#define XR_NOTICE(...)  ((void)(fprintf)(stderr, __VA_ARGS__))
#define XR_OUT(...)     ((void)(printf)(__VA_ARGS__))
/* A whole block of diagnostic work (a loop that builds a table to print). */
#define XR_DIAG_ON()    (g_xr_log_diag != 0)

/* The gate. */
#define fprintf(f, ...)      (xr_log_drop_(f) ? 0 : (fprintf)((f), __VA_ARGS__))
#define vfprintf(f, fmt, ap) (xr_log_drop_(f) ? 0 : (vfprintf)((f), (fmt), (ap)))
#define fputs(s, f)          (xr_log_drop_(f) ? 0 : (fputs)((s), (f)))
#define fputc(c, f)          (xr_log_drop_(f) ? 0 : (fputc)((c), (f)))
#define fwrite(p, sz, n, f)  (xr_log_drop_(f) ? (size_t)0 : (fwrite)((p), (sz), (n), (f)))
#define printf(...)          (g_xr_log_diag ? (printf)(__VA_ARGS__) : 0)
#define puts(s)              (g_xr_log_diag ? (puts)(s) : 0)

#endif /* !__ASSEMBLER__ && !RC_INVOKED */
#endif /* XR_LOG_H */

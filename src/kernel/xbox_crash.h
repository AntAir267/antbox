/*
 * xbox_crash.h - crash and hang reports, one folder each.
 *
 * A report is a directory <save dir>/crash/<YYYYMMDD-HHMMSS>-<kind>/ with
 *
 *   report.txt    what happened: the signal or exception, the host
 *                 backtrace, the guest registers and the guest call stack
 *                 (tools/stackwalk.py's format, and its raw "GS slot word"
 *                 lines, so `stackwalk.py functions.json < report.txt`
 *                 redoes it offline with real function bounds)
 *   log.txt       the last 500 lines of stdout and stderr
 *   settings.txt  the effective settings: what the host passed to
 *                 xbox_crash_set_settings and every RECOMP_* value the
 *                 runtime's table holds
 *   minidump.dmp  Windows only, written by the host
 *   guestram.bin  hang reports only: guest RAM from address 0, raw (64 MB
 *                 on a retail layout); report.txt then also has 256 bytes
 *                 around every guest register that points into it
 *
 * Beside the reports, <save dir>/logs keeps the log of this run and of the
 * three before it (see xbox_crash_set_run_log).
 *
 * The log lines come from a tee: xbox_crash_init points fds 1 and 2 at
 * pipes, and a thread copies what arrives to the original destination and
 * into a ring. Nothing that prints has to know.
 *
 * The hang watchdog counts the title's flips (FLIP_STALL, g_xbox_flip_count)
 * and writes a "hang" report when they stop for RECOMP_HANG_WATCHDOG seconds
 * (default 15, 0 off; 60 before the first flip). The Windows host had a
 * sampler under that name that only printed; this is the same idea on every
 * host, with a report on disk.
 *
 * Writers are meant to be called from a fault handler: no allocation, and on
 * POSIX only calls that are async-signal-safe or, like snprintf and dladdr,
 * safe in practice (as the BLiNX 2 handler this grew from accepts).
 */
#ifndef XBOX_CRASH_H
#define XBOX_CRASH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the log tee and set the report root to <save_dir>/crash. Call once,
 * early, after any stdio redirect (RECOMP_STDIO_LOG) and before the threads
 * that print start. NULL or "" puts reports under ./crash. */
void xbox_crash_init(const char *save_dir);

/* The log tee on its own, before the save folder is known: what is printed
 * from here on is in the ring, and in the run log once xbox_crash_init opens
 * it. Optional; right after the stdio redirect. */
void xbox_crash_log_start(void);

/* The run log (<save dir>/logs/run.log and its tail, the last three runs
 * beside them as prev1..3; the bug report reads them, xbox_report.h). On by
 * default; 0 before xbox_crash_init for a process that is not a play session
 * (a shader pre-build's scene), so it does not push the player's runs out. */
void xbox_crash_set_run_log(int on);
/* <save dir>/logs, or "" when no run log is kept. */
const char *xbox_crash_log_dir(void);

/* The host's part of settings.txt (command line, paths, the enhancements
 * file). Copied; call again to replace it. */
void xbox_crash_set_settings(const char *text);

/* Name this thread for hang samples. xbox_log_thread_role calls it, so every
 * runtime thread is known; a host calls it for its own threads. */
void xbox_crash_thread_register(const char *role);

/* Open a report: the folder, log.txt and settings.txt, and report.txt with
 * `headline` as its first line. 0 if another report is being written (the
 * second fault of a crash) or the folder cannot be made; the text still goes
 * to stderr. */
int xbox_crash_begin(const char *kind, const char *headline);

/* To report.txt and to the original stderr (not the tee: the process may
 * be gone before the tee thread runs again). */
void xbox_crash_printf(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

/* The calling thread's guest registers and guest call stack. */
void xbox_crash_guest_state(void);

/* The guest call stack read from `esp`: return sites in stackwalk.py's
 * format, then the raw words. */
void xbox_crash_guest_stack(uint32_t esp);

/* Name a host code address: "sub_00123456+0x40" or "module+0x..". */
void xbox_crash_host_symbol(const char *label, uintptr_t pc);

#if !defined(_WIN32)
/* The host backtrace of the interrupted context (a signal handler's
 * ucontext_t *): frame records on macOS, the unwinder on Linux. */
void xbox_crash_host_backtrace(void *ucontext);
#endif

/* Close report.txt and say where the report is. Returns the folder, or ""
 * when none was written. */
const char *xbox_crash_end(void);

/* Wait (up to max_ms) for the tee to pass on what is already in its pipes.
 * Before an _exit or ExitProcess that would otherwise cut off the end of
 * the log. Safe in a signal handler. */
void xbox_crash_log_flush(unsigned max_ms);

/* The report directory root (<save dir>/crash). */
const char *xbox_crash_root(void);

/* Bumped at every FLIP_STALL: the hang watchdog's sign of life. */
extern volatile uint32_t g_xbox_flip_count;

/* The Windows host samples every thread itself (dbghelp); it registers a
 * function that prints them with xbox_crash_printf. POSIX hosts need none:
 * the registered threads are sampled by a signal. */
typedef void (*xbox_crash_sampler_fn)(void);
void xbox_crash_set_sampler(xbox_crash_sampler_fn fn);

/* A few lines of a subsystem's own counters in every hang report (up to
 * 8 functions; each prints with xbox_crash_printf). Runs on the watchdog
 * thread while the others are stuck: read, never lock. */
typedef void (*xbox_crash_note_fn)(void);
void xbox_crash_add_note(xbox_crash_note_fn fn);

/* Start the hang watchdog (RECOMP_HANG_WATCHDOG; see above) and arm
 * RECOMP_DEBUG=crash_test. Call once the guest is about to start. */
void xbox_crash_watchdog_start(void);

/* Kernel calls poll this (one load): RECOMP_DEBUG=crash_test=segv[:s] or
 * =hang[:s] fires on the next guest kernel call after s seconds (default 5),
 * on that guest thread. */
extern volatile int g_xbox_crash_test;
void xbox_crash_test_fire(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_CRASH_H */

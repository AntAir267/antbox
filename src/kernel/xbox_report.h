/*
 * xbox_report.h - the bug report: one zip archive a player attaches to an
 * issue.
 *
 * xbox_BugReport writes <save dir>/reports/<name>-report-<time>.zip with
 *
 *   README.txt              what this is, what it holds and what it never holds
 *   system.txt              OS and version, device model, CPU, RAM, the build
 *                           (toolkit and title commits), and what the host
 *                           registers: GPU and driver, displays, controllers,
 *                           the render scale and present mode in effect
 *   settings.toml           the settings file as it is (a host attaches it)
 *   settings-effective.txt  every setting with its source (file, environment,
 *                           default), the runtime's table, the process's own
 *                           RECOMP_ (and the host's) variables
 *   stats.txt               the session's frame times and what else the host
 *                           registers (shader and pipeline compile counts,
 *                           the cache's size)
 *   logs/current.txt        this run's log: its first part and its end
 *   logs/previous-N.txt     the runs before it (xbox_crash.h keeps them)
 *   crash/<report>/...      the newest crash and hang reports (xbox_crash.h)
 *   frame.bmp               the last frame the title presented, when asked for
 *
 * Never in it: save files, disc data, shader or pipeline cache contents
 * (their sizes and counts only), minidumps (their size only), environment
 * variables other than the prefixes the host names (RECOMP_ by default).
 * Nothing is sent anywhere: this file makes no network call, and neither
 * does anything it calls.
 *
 * Privacy: every text member goes through xbox_report_redact before it is
 * compressed. The home directory becomes ~ (and so does any /home/<name>,
 * /Users/<name> or \Users\<name>), the user's login and full name become
 * <user>, the host name <host>. Binary members (the frame) are stored as
 * they are.
 *
 * The archive is a plain zip (deflate, fixed Huffman codes; written here, no
 * library), so anything opens it.
 */
#ifndef XBOX_REPORT_H
#define XBOX_REPORT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── who the text must not name ────────────────────────────────────── */

typedef struct xbox_report_identity {
    const char *home;         /* shown as ~ */
    const char *user;         /* the login name: <user> */
    const char *full_name;    /* the account's real name: <user> */
    const char *host;         /* <host>; its part before the first dot too */
} xbox_report_identity;

/* The process's own: HOME or USERPROFILE, the login and its full name, the
 * host name. The strings live in static storage. */
const xbox_report_identity *xbox_report_identity_self(void);

/* `in` (len bytes) with the identity taken out, NUL-terminated into a
 * malloc'd buffer the caller frees; *out_len is its length. `who` NULL: the
 * process's own. */
char *xbox_report_redact(const xbox_report_identity *who, const char *in, size_t len,
                         size_t *out_len);

/* ── an archive ────────────────────────────────────────────────────── */

typedef struct xbox_report xbox_report;

/* Start an archive at `zip_path` (replaced). `who` NULL: the process's own
 * identity. NULL when the file cannot be made. */
xbox_report *xbox_report_open(const char *zip_path, const xbox_report_identity *who);
/* A text member: redacted, then compressed. 1 when added. */
int xbox_report_text(xbox_report *r, const char *member, const char *text, size_t len);
/* A text file as a member, redacted: all of it, or when it is longer than
 * head + tail its first `head` and last `tail` bytes with a line between
 * saying how much was left out. 0 when the file cannot be read (no member). */
int xbox_report_file(xbox_report *r, const char *member, const char *path,
                     size_t head, size_t tail);
/* A member stored as it is (an image). */
int xbox_report_binary(xbox_report *r, const char *member, const void *data, size_t len);
/* Members so far, and the bytes they held before compression. */
int xbox_report_count(const xbox_report *r);
/* Write the directory and close the file. 1 when the archive is complete. */
int xbox_report_close(xbox_report *r);

/* ── what the runtime and the host add ─────────────────────────────── */

/* A growing text, for the functions below. */
typedef struct xbox_report_buf xbox_report_buf;
void xbox_report_printf(xbox_report_buf *b, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* A directory's size without its contents: the files and bytes under it and
 * one line per entry at its top. */
void xbox_report_dir_summary(xbox_report_buf *b, const char *label, const char *dir);

/* `fn` writes a part of the text member `member` ("system.txt",
 * "settings-effective.txt", "stats.txt", or a name of its own), under
 * `heading`, each time a report is made. In registration order, after what
 * the report writes there itself. Up to 24; call at start. */
typedef void (*xbox_report_section_fn)(xbox_report_buf *b);
void xbox_report_section(const char *member, const char *heading, xbox_report_section_fn fn);

/* A text file to attach as `member`, each time: `path_fn` names it at that
 * moment (NULL or "" then: left out). Up to 8. */
void xbox_report_attach(const char *member, const char *(*path_fn)(void),
                        size_t head, size_t tail);

/* The frame for frame.bmp: `fn` writes the last presented frame as a BMP at
 * `path` within `timeout_ms`, 1 when it did. The runtime's is
 * nv2a_pb_capture_frame (xbox_BugReportRuntime sets it). */
void xbox_report_set_capture(int (*fn)(const char *path, unsigned timeout_ms));
/* Called before the logs are read (the log tee's flush). */
void xbox_report_set_flush(void (*fn)(void));

/* ── the report ────────────────────────────────────────────────────── */

typedef struct xbox_bug_report_opts {
    const char *name;         /* the file name's first part: "burnout3" */
    const char *title;        /* README's first line: the title and its build */
    const char *save_dir;     /* holds logs/, crash/ and reports/ */
    const char *out_dir;      /* NULL: <save_dir>/reports */
    const char *log_dir;      /* NULL: <save_dir>/logs */
    const char *crash_dir;    /* NULL: <save_dir>/crash */
    const char *const *env_prefixes;  /* NULL-terminated; NULL: RECOMP_ only */
    const char *note;         /* a line for README (where it was made from) */
    int screenshot;           /* 1: frame.bmp, when a frame can be had */
    int no_current_run;       /* 1: no title runs in this process (a command
                               * line or start-page report): the newest log
                               * is the last run's, not this one's */
    const xbox_report_identity *who;  /* NULL: the process's own (tests) */
} xbox_bug_report_opts;

/* Write the archive. 1 with its path in `out_path`; 0 with a reason there. */
int xbox_BugReport(const xbox_bug_report_opts *o, char *out_path, size_t n);

/* Remember which of the process's variables were set when it started, so the
 * report can tell "environment" from a default the host sets later. First
 * thing in main, before any setenv. */
void xbox_report_env_snapshot(void);

/* The runtime's own parts, registered: the session's flip statistics, the
 * frame capture, the log flush. A host with the kernel linked calls it once
 * at start. (Not in a build of this file alone, XBOX_REPORT_STANDALONE.) */
void xbox_BugReportRuntime(void);

/* The host's way to make one: it fills the options (its name, its save
 * folder, its variables' prefixes) and calls xbox_BugReport. Whoever offers
 * a report asks through this: the settings overlay's action, the debug
 * switch below, an Android start page. */
typedef int (*xbox_bug_report_fn)(int screenshot, char *out_path, unsigned n);
void xbox_BugReportSetMaker(xbox_bug_report_fn fn);
int  xbox_BugReportHasMaker(void);
/* 1 with the archive's path in out_path; 0 with a reason there. */
int  xbox_BugReportMake(int screenshot, char *out_path, unsigned n);

/* RECOMP_DEBUG=bug_report=<seconds>[:shot]: the maker is called that long
 * after this, on a thread of its own, and the result printed
 * ("[REPORT] bug_report: done: <path>"): a report without a window, for
 * tests and for a machine reached over ssh. =@<file>: a report each time
 * that file appears instead (it is removed; "shot" in it asks for the
 * frame), so a script picks the moment. "paused" beside "shot" (":shot:paused",
 * or in the file) pauses the title around the report, as the overlay does.
 * Call when the title is about to start. */
void xbox_BugReportArm(void);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_REPORT_H */

/*
 * xbox_autoplay.h - a title driven through its screens by a script, from
 * inside the process.
 *
 * What a title's headless scene drivers do from outside (they read the log
 * and append presses to RECOMP_PAD_LIVE), as a thread of the process: a list
 * of steps that wait on what the title does, never on the clock, so the
 * same script holds at any frame rate.
 *
 *   files    the title opening a file (xbox_FileOpenHook): a movie, a car,
 *            a track. The scene changes are anchored on those.
 *   flips    the executor's flips (g_xbox_flip_count): a title steps its
 *            menus once a frame, so "n flips" is the same moment of the
 *            game on any machine.
 *   presses  lines appended to the RECOMP_PAD_LIVE file, each waited for
 *            until the pad has taken it (usb_gamepad_live_taken).
 *
 * Nothing here knows a title: the steps are the host's.
 *
 * Its first user is a shader pre-build: the title is played through its
 * reference scenes with nobody at it, so the GPU backend meets, compiles
 * and lists the shaders and pipelines those scenes need (nv2a_pcache.h).
 *
 * The host sets RECOMP_PAD_LIVE to the file named here before the title
 * starts (the pad reads the variable once), then calls xbox_AutoplayStart.
 */
#ifndef XBOX_AUTOPLAY_H
#define XBOX_AUTOPLAY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XAP_END = 0,
    /* Until the title has opened a file whose path contains `s` (ASCII,
     * case-insensitive, '/' and '\\' the same), counted from the last
     * XAP_SYNC. Sets the anchor to the flip of that open. With n > 0: for n
     * flips at most, then the run goes on with the anchor at that flip (an
     * event that is usually marked by a file, but need not be). */
    XAP_WAIT_FILE,
    /* Press `s` ("a:400", "down:200", "rt:30000": button[+button][:hold ms],
     * as RECOMP_PAD_LIVE), wait until the pad took it, then `n` flips. */
    XAP_PRESS,
    /* Press `s`, and again every `n` flips, until the title opens a file
     * containing `s2`: for a screen that ignores input while it slides in. */
    XAP_PRESS_UNTIL,
    XAP_WAIT_FLIPS,         /* n flips from now */
    XAP_WAIT_ANCHOR,        /* until n flips after the anchor */
    /* Only file opens from here on count for the waits that follow: at the
     * start of a part that opens files an earlier part already opened. */
    XAP_SYNC,
    /* A named point for the progress callback: `s` its text, `n` the
     * progress reached there, in thousandths. */
    XAP_MARK
} xap_op;

typedef struct xap_step {
    xap_op      op;
    const char *s, *s2;
    int         n;
} xap_step;

typedef struct xap_run {
    const xap_step *steps;          /* ends with XAP_END */
    const char *pad_file;           /* the RECOMP_PAD_LIVE file */
    /* Lines of that file the pad has taken: usb_gamepad_live_taken (passed
     * in, as the kernel library does not link the USB one). */
    unsigned (*pad_taken)(void);
    int wait_timeout_s;             /* the longest one step may wait; 0: 300 */
    /* Called on the driver's thread. `done`: ok 1 at XAP_END, 0 with the
     * step that timed out in `why`, -1 when cancelled. */
    void (*mark)(void *ctx, const char *text, int thousandths);
    void (*done)(void *ctx, int ok, const char *why);
    /* Polled a few times a second; nonzero cancels the run. May be NULL. */
    int  (*cancelled)(void *ctx);
    void *ctx;
} xap_run;

/* Starts the driver thread; `run` and what it points to must stay valid.
 * Installs xbox_FileOpenHook. 1 started, 0 not (a thread could not be made,
 * or one is running). */
int xbox_AutoplayStart(const xap_run *run);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_AUTOPLAY_H */

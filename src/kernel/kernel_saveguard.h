/*
 * kernel_saveguard.h - a title's save files are replaced, not overwritten.
 *
 * A console title writes its save in place: it opens the file, sets its
 * size, writes, closes. On the console the worst an interruption costs is
 * that one save. On a PC the process can be killed, the battery can run out
 * and the disk can fill at any of those steps, and what is left is a file
 * of the right name with half a save in it.
 *
 * With the guard on, a file under the title's save trees (UDATA\<id>,
 * TDATA\<id>) that is opened for writing is not the file itself but a new
 * file beside it, "<name>.~new", which starts as a copy of the old one.
 * The title writes there. When its last handle closes, a thread of the
 * guard's own flushes the new file to the disk and renames it over the old
 * name. The old file is whole until that rename and the new one is whole
 * after it; there is no moment with half of each.
 *
 * Off unless the port turns it on; with it off, the file layer does
 * exactly what it did.
 *
 * What it does not cover: a file the title renames by handle while it is
 * being written keeps its old name's file as it was (the renamed one is
 * the new name's file); a handle opened relative to a directory handle on
 * Windows is written in place, as before.
 */
#ifndef KERNEL_SAVEGUARD_H
#define KERNEL_SAVEGUARD_H

#include "kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── the port ──────────────────────────────────────────────────────── */

/* Guard the save trees of this title id (eight hex digits, as the folder
 * is named). Once, after the path table is set up and before the title
 * runs. A new file left over from a process that ended mid-save is removed
 * here: the file beside it is the save. */
void xbox_SaveGuardEnable(const char *title_id);

/* Called on the guard's thread right before a changed file takes the old
 * one's place: the port's moment to keep the save as it still is. Not
 * called when the new file has the same bytes as the old. */
void xbox_SaveGuardSetHook(void (*before_replace)(void));

/* How many files are written but not yet in place (0: a quit loses
 * nothing). */
int xbox_SaveGuardBusy(void);

/* Wait, at most `ms`, until that is 0. 1 when it is. The guard does it
 * itself, up to two seconds, on every way out (atexit, and the exit hooks
 * of the windows that end with ExitProcess). */
int xbox_SaveGuardWait(unsigned ms);

/* A save that could not be put in place (Windows: another program held the
 * old file, so the rename over it failed). The newer file is never thrown
 * away: it is kept beside the old one ("<name>.~pend"), reads of the name see
 * it, and the rename is tried again at the next save's close and on the way
 * out; what is still pending at the end is put in place by the next start
 * (or, when it cannot be, kept in the save history and pending again).
 * After a few failed tries this returns a sequence number (> 0, new for each
 * new failure) and the text for the player in `out`; 0 while nothing is
 * wrong. Any thread. */
int xbox_SaveGuardProblem(char *out, unsigned n);

/* How many saves wait to be put in place (above). */
int xbox_SaveGuardPending(void);

/* Tests: the next `n` renames of a new save over the old one fail, as a
 * Windows rename fails while another program holds the old file. */
void xbox_SaveGuardFailReplace(int n);

/* Tests: end the process (_exit, code 90 + step) at this step of the next
 * save. 0 turns it off. */
#define XBOX_SAVE_FAULT_AFTER_OPEN      1   /* the new file is there, a copy   */
#define XBOX_SAVE_FAULT_AFTER_WRITE     2   /* after the title's first write   */
#define XBOX_SAVE_FAULT_BEFORE_FLUSH    3   /* closed, nothing flushed         */
#define XBOX_SAVE_FAULT_BEFORE_REPLACE  4   /* flushed, not renamed            */
#define XBOX_SAVE_FAULT_AFTER_REPLACE   5   /* renamed                         */
void xbox_SaveGuardFaultAt(int step);

/* ── the file layer (kernel_file.c) ────────────────────────────────── */

/* Before a file is opened. `host_path` is the translated path; when the
 * file is guarded and the open writes, it is rewritten to the new file's
 * path and a token comes back for xbox_saveguard_opened. NULL: open as
 * always. An open of a file whose new version is still on its way to the
 * disk waits for it here. */
void *xbox_saveguard_open(xbox_host_char *host_path, unsigned n, int write_access, int directory);

/* After that open: the handle it gave, or ok = 0 when it failed. */
void xbox_saveguard_opened(void *token, HANDLE h, int ok);

/* The title wrote to, or set the size of, this handle. */
void xbox_saveguard_wrote(HANDLE h, unsigned bytes);

/* The title deleted the file through this handle (delete on close): the
 * file of the real name goes with it. */
void xbox_saveguard_deleted(HANDLE h);

/* Before the handle is closed. On a guarded file's last handle this hands
 * the new file to the guard's thread and returns at once. */
void xbox_saveguard_close(HANDLE h);

/* Before a look at a guarded path or folder by name (attributes, a
 * listing, a delete): waits until nothing is on its way to the disk, so
 * the answer is the one after the save. */
void xbox_saveguard_settle(const xbox_host_char *host_path);

/* 1 for a name the title must not see in a listing ("<name>.~new",
 * "<name>.~pend"). */
int xbox_saveguard_hidden_name(const xbox_host_char *name);

#ifdef __cplusplus
}
#endif

#endif

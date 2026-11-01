/*
 * recomp_save_dir.h - where a title's saves live, and their backups.
 *
 * The save root holds what the kernel maps there (UDATA, TDATA, the HDD
 * partition images; kernel_path.c) and the crash reports (xbox_crash.h). By
 * platform, as other ports keep theirs:
 *
 *   macOS    ~/Library/Application Support/<Name>
 *   Linux    $XDG_DATA_HOME/<name>, else ~/.local/share/<name>
 *   Windows  %APPDATA%\<Name>  (under Proton: the prefix's
 *            drive_c/users/steamuser/AppData/Roaming/<Name>)
 *
 * RECOMP_SAVE_DIR=<dir> replaces it, which is how a test run keeps its own.
 */
#ifndef RECOMP_SAVE_DIR_H
#define RECOMP_SAVE_DIR_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The save root, created: RECOMP_SAVE_DIR, else the platform directory for
 * `name` (lower-cased on Linux). Resolved once; the same string after. */
const char *recomp_save_dir(const char *name);

/* A per-user folder by platform, not created. 1 with the path in `out`.
 *
 *   RECOMP_DIR_DATA   the save root above, without RECOMP_SAVE_DIR
 *   RECOMP_DIR_LOCAL  for what is large and stays on this machine (a title's
 *                     data): Windows %LOCALAPPDATA%\<Name>; elsewhere the same
 *                     folder as RECOMP_DIR_DATA
 *   RECOMP_DIR_CACHE  macOS ~/Library/Caches/<Name>, Linux $XDG_CACHE_HOME/<name>
 *                     else ~/.cache/<name>, Windows %LOCALAPPDATA%\<Name>\cache
 *                     (where nv2a_pcache.h keeps the shader cache)
 */
enum { RECOMP_DIR_DATA, RECOMP_DIR_LOCAL, RECOMP_DIR_CACHE };
int recomp_user_dir(int kind, const char *name, char *out, unsigned out_size);

/* A folder that got a new name: move `old_dir` to `new_dir`, once. Only when
 * `old_dir` is a folder and nothing is at `new_dir` (a folder without a file
 * in it is nothing); no file is ever deleted or written over, so a second call, or
 * both being there, changes nothing.
 * 1 moved, 0 nothing to do, -1 it could not be moved (both are as they
 * were). Call it before anything makes `new_dir` (recomp_save_dir does). */
int recomp_dir_migrate(const char *old_dir, const char *new_dir);
/* Whether recomp_dir_migrate would move it: `old_dir` is a folder (not a
 * link) and `new_dir` is not there, or is there with no file anywhere in it
 * (such a one gives way: something that only asked where the folder is may
 * have made it, and an empty cache folder inside it).
 * Reads only. */
int recomp_dir_migrate_pending(const char *old_dir, const char *new_dir);

/* Copy a save folder from before this (the working directory's saves/) into
 * `dir`, once: only when `legacy` has a UDATA, `dir` has none and `legacy`
 * has no MOVED.txt from an earlier copy. The
 * partition images are not copied (sparse, and remade at start); the legacy
 * folder is left as it was, with a MOVED.txt naming the new place.
 * 1 when it copied. */
int recomp_save_dir_migrate(const char *legacy, const char *dir);

/* Copy dir/UDATA and dir/TDATA to dir/backup/<YYYYMMDD-HHMMSS>/, unless they
 * are byte for byte the newest backup, and keep the `keep` newest backups.
 * Call at start, before the title can write. Fills `out` (may be NULL) with
 * the backup made, or "" when none was needed. 1 when it made one. */
int recomp_save_backup(const char *dir, int keep, char *out, unsigned out_size);

/* ── a save out of the profile and back ────────────────────────────── */

/*
 * An export is a folder holding the title's own save files as they are in
 * the save root, UDATA/<title id>/... and TDATA/<title id>/... (title_id as
 * the title names its folder: eight hex digits), and a small text file
 * (save-export.txt: "title <id>", when it was made, a line of the port's).
 * The layout under the save root is the same on every host, so an export
 * made on one imports on another. Nothing else of the save root goes along
 * (other titles' folders, the dashboard's music database, partition images,
 * backups, settings).
 *
 * An import is not done under a running title: the title holds its save in
 * memory once a profile is loaded and writes it back when it saves. It is
 * staged (a note in the save root) and applied by recomp_save_import_apply
 * at the next start, before the title runs: the save in use is copied to
 * <save root>/save-before-import/<date-time>/ first (never deleted by the
 * start-up backups' rotation), then replaced; if the copy fails it is put
 * back. Paths are UTF-8.
 */

/* ── a cheat session's copy of the save ─────────────────────────────── */

/*
 * While a cheat is on (xbox_OverlaySetCheats), the title's saves go to a copy
 * (xbox_PathSetUserRoot), so its UDATA and TDATA are not written: begin copies
 * root/UDATA and root/TDATA to `sandbox` (anything there before is removed
 * first), commit copies them back over the root (each file written beside
 * the old one, flushed, and renamed over it: a file of the save is the old
 * one or the new one, whole, whatever happens; the caller takes a history
 * copy of the save first, recomp_save_history_take), discard removes the copy.
 * Paths UTF-8. 1 when every file was copied.
 */
int recomp_save_sandbox_begin(const char *root, const char *sandbox);
int recomp_save_sandbox_commit(const char *sandbox, const char *root);
void recomp_save_sandbox_discard(const char *sandbox);

/* 1 when the save root has files of this title to export. */
int recomp_save_has_data(const char *dir, const char *title_id);
/* Copy the save to <dest_root>/<YYYYMMDD-HHMMSS>/ (made as needed; `note`
 * is a line for save-export.txt, may be NULL). 1 with the folder in `out`;
 * 0 with a sentence for the player in `err`. */
int recomp_save_export(const char *dir, const char *title_id, const char *dest_root, const char *note,
                       char *out, unsigned out_size, char *err, unsigned err_size);
/* Whether `folder` is an export of this title (or a hand copy with the same
 * shape): it has UDATA/<title id> with a file in it, and its save-export.txt,
 * if any, names this title. 0 with a sentence in `err` (may be NULL). */
int recomp_save_export_check(const char *folder, const char *title_id, char *err, unsigned err_size);
/* The newest export of this title in `dest_root` (by its dated name). */
int recomp_save_export_newest(const char *dest_root, const char *title_id, char *out, unsigned out_size);

/* Ask for `folder` to be imported at the next start (checked first). */
int recomp_save_import_stage(const char *dir, const char *folder, const char *title_id,
                             char *err, unsigned err_size);
/* 1 and the folder when an import is asked for. */
int recomp_save_import_staged(const char *dir, char *folder, unsigned size);
void recomp_save_import_cancel(const char *dir);
/* At start, before the title and before anything else looks at the save
 * root: do the import asked for. 0 nothing was asked, 1 imported, -1 not
 * imported (the save in use is as it was). `report` gets a sentence for the
 * log; the same is kept for recomp_save_import_result until the next start. */
int recomp_save_import_apply(const char *dir, const char *title_id, char *report, unsigned size);
int recomp_save_import_result(const char *dir, char *text, unsigned size);

/* ── the save history ──────────────────────────────────────────────── */

/*
 * <save root>/save-history/<YYYYMMDD-HHMMSS>/: the save as it was right
 * before the game wrote over it, each an export (above), so one is put back
 * the way an export is (recomp_save_import_stage). The port takes one from
 * the save guard's hook (kernel_saveguard.h), once for the files one save
 * writes together. Kept: the RECOMP_SAVE_HISTORY_NEWEST newest, and the
 * newest of each of the last RECOMP_SAVE_HISTORY_DAYS days and of each of
 * the last RECOMP_SAVE_HISTORY_WEEKS weeks. Only dated, real folders are
 * ever removed; anything else in there is left alone.
 */
#define RECOMP_SAVE_HISTORY_NEWEST 20
#define RECOMP_SAVE_HISTORY_DAYS    7
#define RECOMP_SAVE_HISTORY_WEEKS   4

/* Keep the save as it is now. 1 with the folder in `out`. Prunes after. */
int recomp_save_history_take(const char *dir, const char *title_id, char *out, unsigned out_size);
/* Remove what is not kept, as of `now`. How many it removed. */
int recomp_save_history_prune(const char *dir, time_t now);
/* Which of `names` (sorted, oldest first) are kept as of `now`: keep[i] 1. */
void recomp_save_history_keep(const char *const *names, int n, time_t now, int *keep);
/* The folder (1 when it is there). */
int recomp_save_history_dir(const char *dir, char *out, unsigned size);
/* How many saves of this title it holds. */
int recomp_save_history_count(const char *dir, const char *title_id);

/* The player's home folder: $HOME, %USERPROFILE% on Windows; on Android the
 * app's own external files directory (the one place it may write without a
 * permission). Not created. */
int recomp_home_dir(char *out, unsigned size);

/* Small file helpers, for code that cannot reach this file's own (the Xbox
 * save importer). Paths UTF-8, '/' accepted everywhere. fn returns 0 to stop. */
int recomp_fs_is_dir(const char *path);
void recomp_fs_mkdir_p(const char *path);
void recomp_fs_rm_tree(const char *path);
int recomp_fs_copy_tree(const char *from, const char *to);
int recomp_fs_each(const char *dir, int (*fn)(const char *dir, const char *name, int dir_entry, void *ctx),
                   void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_SAVE_DIR_H */

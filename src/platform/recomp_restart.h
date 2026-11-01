/*
 * recomp_restart.h - start the program again, as it was started.
 *
 * For a setting the runtime reads once at start (the render scale, the
 * speaker layout ...) or a save put in place before the title runs: the
 * program ends the ordinary way and comes back with the same arguments, in
 * the same directory, with the environment it was given.
 *
 * "As it was given" matters: a host puts its own defaults and the settings
 * file's values into the environment while it starts (env_default,
 * xbox_SettingsInit), and a program started with THAT environment would take
 * every one of them for the user's own variable. So recomp_restart_init, the
 * first call in main, copies the arguments, the working directory and the
 * environment, and registers the handler that does the restart with atexit
 * before anything else registers one: it therefore runs last, after the
 * settings are written, the pads' motors are stopped, the audio device is
 * closed and the log is finished, exactly as at any quit.
 *
 *   Linux, macOS   execve in place: the same process id. No second
 *                  instance at any moment; Steam still sees the game it
 *                  started, a launcher script that waits for it keeps
 *                  waiting, an AppImage's runtime (the parent holding the
 *                  mount) stays, a Mac bundle's executable is the same file.
 *   Windows        no exec in place: CreateProcessW with the same wide
 *                  command line, the saved environment block and directory,
 *                  as the last thing the old process does.
 *   Android        the activity restarts the app (a hook the window layer
 *                  installs: recomp_restart_set_hook); the process ends
 *                  there, not through exit.
 */
#ifndef RECOMP_RESTART_H
#define RECOMP_RESTART_H

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in main (after the arguments are final: on Windows after
 * recomp_win_utf8_args). Without it a restart is not available. */
void recomp_restart_init(int argc, char **argv);
/* Windows: a run started by a restart waits in recomp_restart_init for
 * the run that started it to end (never two at once on the save folder);
 * one line saying so, for the log once it is open ("" otherwise). */
const char *recomp_restart_note(void);
int  recomp_restart_available(void);

/* Ask for it: the next ordinary exit (exit(), main returning) starts the
 * program again instead of just ending. The caller then quits the usual way
 * (closes the window: an SDL_QUIT event). `why` goes to the log. On a host
 * with a hook (Android) the hook is called and does it all. */
void recomp_restart_request(const char *why);
int  recomp_restart_requested(void);
/* Take the request back (nothing was quit yet). */
void recomp_restart_cancel(void);

/* A host whose restart is not an exec (Android: the activity). */
void recomp_restart_set_hook(void (*fn)(void));

/* Where stdout and stderr went before a log capture took them over
 * (xbox_crash.c's tee puts them on pipes it reads): descriptors holding the
 * original destinations, -1 for a stream left alone. An exec in place keeps
 * descriptors 1 and 2, and the pipes behind them have no reader in the new
 * run: its first line to stderr ends it with SIGPIPE. The restart puts these
 * back first. */
void recomp_restart_stdio(int out_fd, int err_fd);

/* Quit for good (the overlay's "Quit game"): the caller closes the window the
 * ordinary way (an SDL_QUIT event: settings written, pads stopped, the
 * window layer's own wait for the saves, then exit(0)). This only runs the
 * host's hook first, if it has one: on Android the activity is finished, so
 * the task does not stay behind in the recents or come back on its own. */
void recomp_quit_set_hook(void (*fn)(void));
void recomp_quit_request(const char *why);

/* Tests: what the exec would start (off Windows): the file, its arguments
 * and environment. In an AppImage the image file (APPIMAGE), named as its
 * runtime was (ARGV0), without the runtime's variables for the old mount;
 * otherwise the program itself with the arguments and environment as
 * given. */
void recomp_restart_target(const char **exe, char ***argv, char ***envp);

/* Tests: what would be started. Pointers into the module's copies; envp is
 * NULL-terminated "NAME=value" strings. */
void recomp_restart_snapshot(int *argc, char ***argv, char ***envp, const char **cwd);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_RESTART_H */

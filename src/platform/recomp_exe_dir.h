/*
 * recomp_exe_dir.h - the directory that holds the running executable.
 *
 * Runtime configuration that belongs to an installed game (enhance.toml)
 * sits beside the executable, not in the working directory, so a launcher
 * or a shortcut that starts the game from elsewhere still finds it.
 */
#ifndef RECOMP_EXE_DIR_H
#define RECOMP_EXE_DIR_H

#ifdef __cplusplus
extern "C" {
#endif

/* The executable's directory, without a trailing separator ("." when it
 * cannot be found, i.e. the working directory). Resolved once and cached;
 * call it first from one thread (startup). */
const char *recomp_exe_dir(void);

/* The executable's own path, for a host that starts itself again (a shader
 * pre-build runs the title once per scene). "" when it could not be found. */
const char *recomp_exe_path(void);

/* Portable mode (Windows and Linux). A file "portable.txt", or a folder
 * "data", next to the program puts what is kept per user -- saves, settings,
 * logs, the game's own data, the shader cache -- in <that folder>/data instead
 * of %APPDATA% and %LOCALAPPDATA%, or the XDG folders (recomp_save_dir.h:
 * recomp_user_dir). Next to the program is the executable's folder; for a
 * Linux AppImage (APPIMAGE set) the folder of the .AppImage file, because the
 * executable is inside a read-only mount. Without the marker nothing changes.
 * The folders are made when the program needs them.
 *
 * recomp_portable_find is the pure test, for any host and any folder: 1 with
 * "<dir>/data" in `out` when the marker is there, else 0.
 * recomp_portable_base picks the folder to test: the folder of `appimage`
 * when it is set, else `exe_dir`.
 * recomp_portable_root asks both about this program, once, and answers 0 on
 * the other platforms (macOS, Android). */
int recomp_portable_find(const char *exe_dir, char *out, unsigned out_size);
int recomp_portable_base(const char *appimage, const char *exe_dir, char *out, unsigned out_size);
int recomp_portable_root(char *out, unsigned out_size);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_EXE_DIR_H */

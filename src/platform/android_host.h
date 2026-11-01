/*
 * android_host.h - what an Android host needs that the POSIX layer has not.
 *
 * An Android app is a shared library in a process the system started: there
 * is no terminal behind stdout, the working directory is "/", HOME names
 * nothing writable and /proc/self/exe is the system's app_process. The
 * title's activity (Java) knows the app's directories and passes them in the
 * environment before the native code starts; this file does the rest.
 *
 * The environment the activity sets (android.system.Os.setenv, before
 * SDLActivity loads the libraries):
 *
 *   RECOMP_ANDROID_DIR   the app's own external files directory
 *                        (Context.getExternalFilesDir(null), e.g.
 *                        /storage/emulated/0/Android/data/<package>/files):
 *                        the working directory, where the title's data folder
 *                        and enhance.toml are; readable over adb and MTP
 *   XDG_DATA_HOME        the same directory: recomp_save_dir() puts the save
 *                        folder under it, as on Linux
 *   XDG_CACHE_HOME       Context.getCacheDir(): the shader cache (the system
 *                        may empty it when storage runs short)
 *   RECOMP_ANDROID_DRIVER_DIR / RECOMP_ANDROID_LIB_DIR
 *                        reserved for a replacement GPU driver (not read yet)
 *
 * Android only; the header is empty elsewhere.
 */
#ifndef RECOMP_ANDROID_HOST_H
#define RECOMP_ANDROID_HOST_H

#if defined(__ANDROID__)

#ifdef __cplusplus
extern "C" {
#endif

/* First thing in the host's main: stdout and stderr go to logcat under `tag`
 * and to <RECOMP_ANDROID_DIR>/<logname> (the previous one is kept as
 * <logname>.1; logname NULL: logcat only), and the working directory becomes
 * RECOMP_ANDROID_DIR. Returns that directory, or NULL when the activity did
 * not name one (the working directory is then left alone). */
const char *recomp_android_host_init(const char *tag, const char *logname);

/* RECOMP_ANDROID_DIR, or NULL. */
const char *recomp_android_dir(void);

/* The core with the highest maximum frequency among those this process may
 * use (cpufreq's cpuinfo_max_freq; the highest-numbered of equals, which on
 * the usual little/big/prime layout is the prime core), or -1 when cpufreq
 * cannot be read. */
int recomp_android_fast_cpu(void);

/* The same by rank: 0 the fastest core, 1 the highest-numbered core of the
 * next slower kind (the "big" cluster under a "prime" core), and so on;
 * -1 when there is no such kind. */
int recomp_android_cpu_rank(int rank);

#ifdef __cplusplus
}
#endif

#endif /* __ANDROID__ */
#endif /* RECOMP_ANDROID_HOST_H */

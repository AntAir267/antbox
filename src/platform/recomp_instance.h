/*
 * recomp_instance.h - one running copy of the program per save root.
 *
 * Two copies on one save root each hold the title's save in memory and each
 * write it back when they save: the later one wins and the other's progress
 * is lost without a word. A lock file in the save root ("instance.lock",
 * flock / an exclusive open on Windows) is held while the program runs; a
 * second copy finds it held and says so instead of starting.
 *
 * The lock goes with the process (a crash, a kill), so there is nothing to
 * clean up. A restart (recomp_restart.h) gets it back: the exec closes it
 * with the old image, and a new process waits a few seconds for the old one
 * to end (Windows, where the old one ends after the new one starts).
 */
#ifndef RECOMP_INSTANCE_H
#define RECOMP_INSTANCE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Take the lock for `save_root`, waiting up to `wait_ms` for it.
 * 1 taken (held until the process ends), 0 another copy holds it,
 * -1 no lock file could be made (a read-only folder: the program runs, as
 * it did before there was a lock). */
int recomp_instance_lock(const char *save_root, unsigned wait_ms);

/* Let it go (tests). */
void recomp_instance_unlock(void);

#ifdef __cplusplus
}
#endif

#endif

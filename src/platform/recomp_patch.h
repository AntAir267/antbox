/*
 * recomp_patch.h - the runtime half of the opt-in guest patches.
 *
 * tools.recomp --patches (tools/recomp/patches.py) follows a listed guest
 * load with
 *
 *     reg = RECOMP_PATCH_U32("game.speed_units", "kmh", 2u, reg);
 *
 * which lands in recomp_patch_u32: the value the guest loaded comes back
 * unchanged unless the setting named by `key` currently reads `when`, then
 * `value` does (`when` "!off": the setting is set and reads anything else). A host opts a setting in with recomp_patch_enable, naming
 * the environment variable that carries it; that variable is the one the
 * overlay and settings.toml drive (xbox_settings.h), so a change is seen by
 * the next load, and the environment (RECOMP_SPEED_UNITS=kmh) works with no
 * overlay built. A setting nobody enabled is "off": the title runs as it was.
 *
 * A float patch ("load_f32", "store_f32") carries an expression instead of a
 * constant:
 *
 *     xmm0.f[0] = RECOMP_PATCH_F32("display.hud_fix", "1", 0,
 *         ((double)(xmm0.f[0]) + (640.0 - 480.0 * recomp_patch_var("aspect")) / 2.0),
 *         xmm0.f[0]);
 *
 * evaluated only while the setting is on. The names in it are the host's:
 * recomp_patch_set_var("aspect", 16.0 / 9.0), before the guest runs.
 */
#ifndef RECOMP_PATCH_H
#define RECOMP_PATCH_H

#include <stdint.h>

#include "recomp_env.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opt the setting `key` in; its value is the cached environment variable
 * `env`. Call from the host's main, before the guest runs. */
void recomp_patch_enable(const char *key, recomp_env_id env);
/* The setting was enabled (the overlay shows its row only then). */
int recomp_patch_enabled(const char *key);
/* A latched pair: a "set" site (the title builds something from the setting,
 * once) samples the setting; "use" sites (it reads the same choice every
 * frame) follow that sample, so a change made mid-way shows together, at the
 * next "set", and never as half a display. Before any "set" a "use" site
 * follows the setting. */
enum { RECOMP_PATCH_LATCH_SET = 1, RECOMP_PATCH_LATCH_USE = 2 };
uint32_t recomp_patch_u32_latch(const char *key, const char *when,
                                uint32_t value, uint32_t loaded, int mode);
/* The setting `key` reads `when` (mode 0), with the latch (mode 1 set, 2
 * use). What the float patches' macros test before they evaluate. */
int recomp_patch_on(const char *key, const char *when, int mode);
/* A name a float patch's expression may read. Call from the host's main,
 * before the guest runs. A name nobody set reads 0. Up to 64 names (a
 * name past that is said on stderr and reads 0). */
void recomp_patch_set_var(const char *name, double value);
/* The same for a value that can change while the title runs (what part of
 * the frame the window shows): `fn` is called at each evaluation, from the
 * guest's threads. */
void recomp_patch_set_var_fn(const char *name, double (*fn)(void));
int recomp_patch_has_var(const char *name);
double recomp_patch_var(const char *name);
/* A "mark" patch (tools/recomp/patches.py): `fn` runs, on the guest's
 * thread, each time the title reaches a site marked `name`. Call from the
 * host's main, before the guest runs. recomp_patch_mark is what the
 * generated code calls; a name nobody registered costs a short loop. Up
 * to 64 marks. */
void recomp_patch_on_mark(const char *name, void (*fn)(void));
void recomp_patch_mark(const char *name);
/* What the generated code calls. Safe from any thread. */
uint32_t recomp_patch_u32(const char *key, const char *when, uint32_t value,
                          uint32_t loaded);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_PATCH_H */

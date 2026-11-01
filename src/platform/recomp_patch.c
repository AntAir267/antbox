/* recomp_patch.c - see recomp_patch.h. */
#include "recomp_patch.h"

#include <stdio.h>
#include <string.h>

#define PATCH_MAX 32

static struct {
    const char *key;
    recomp_env_id env;
    volatile int latched;          /* -1: no "set" site ran yet, else 0/1 */
} s_en[PATCH_MAX];
static volatile int s_n;   /* written once, before the guest runs */

void recomp_patch_enable(const char *key, recomp_env_id env)
{
    int i;
    for (i = 0; i < s_n; i++)
        if (!strcmp(s_en[i].key, key)) {
            s_en[i].env = env;
            return;
        }
    if (s_n == PATCH_MAX) {
        fprintf(stderr, "[PATCH] switch table full (%d): %s is not registered\n", PATCH_MAX, key);
        return;
    }
    s_en[s_n].key = key;
    s_en[s_n].env = env;
    s_en[s_n].latched = -1;
    s_n = s_n + 1;
}

int recomp_patch_enabled(const char *key)
{
    int i, n = s_n;
    for (i = 0; i < n; i++)
        if (!strcmp(s_en[i].key, key))
            return 1;
    return 0;
}

int recomp_patch_on(const char *key, const char *when, int mode)
{
    int i, n = s_n;
    for (i = 0; i < n; i++)
        if (s_en[i].key == key || !strcmp(s_en[i].key, key)) {
            const char *v = recomp_env(s_en[i].env);
            /* "!x": set, and anything but x (a choice with several "on"s). */
            int on = when[0] == '!' ? (v && *v && strcmp(v, when + 1) != 0)
                                    : (v && !strcmp(v, when));
            if (mode == RECOMP_PATCH_LATCH_SET)
                s_en[i].latched = on;
            else if (mode == RECOMP_PATCH_LATCH_USE && s_en[i].latched >= 0)
                on = s_en[i].latched;
            return on;
        }
    return 0;
}

uint32_t recomp_patch_u32_latch(const char *key, const char *when,
                                uint32_t value, uint32_t loaded, int mode)
{
    return recomp_patch_on(key, when, mode) ? value : loaded;
}

uint32_t recomp_patch_u32(const char *key, const char *when, uint32_t value,
                          uint32_t loaded)
{
    return recomp_patch_u32_latch(key, when, value, loaded, 0);
}

/* The names a float patch's expression reads. Set before the guest runs,
 * read from its threads: a slot is written whole before the count moves.
 * A name that finds the table full is said on stderr: a dropped name reads
 * 0 in every expression (BO-245: int027's two rival-marker names filled the
 * old 16 slots and fe_dw, registered last, read 0, so the front end's
 * full-frame decoration and the films drew at the left in 16:9). */
#define VAR_MAX 64
static struct {
    const char *name;
    double value;
    double (*fn)(void);            /* a live value, or NULL: `value` */
} s_var[VAR_MAX];
static volatile int s_nvar;

static void var_set(const char *name, double value, double (*fn)(void))
{
    int i;
    for (i = 0; i < s_nvar; i++)
        if (!strcmp(s_var[i].name, name)) {
            s_var[i].value = value;
            s_var[i].fn = fn;
            return;
        }
    if (s_nvar == VAR_MAX) {
        fprintf(stderr, "[PATCH] name table full (%d): %s is not registered, its expressions read 0\n",
                VAR_MAX, name);
        return;
    }
    s_var[s_nvar].name = name;
    s_var[s_nvar].value = value;
    s_var[s_nvar].fn = fn;
    s_nvar = s_nvar + 1;
}

void recomp_patch_set_var(const char *name, double value)
{
    var_set(name, value, NULL);
}

void recomp_patch_set_var_fn(const char *name, double (*fn)(void))
{
    var_set(name, 0.0, fn);
}

int recomp_patch_has_var(const char *name)
{
    int i, n = s_nvar;
    for (i = 0; i < n; i++)
        if (s_var[i].name == name || !strcmp(s_var[i].name, name))
            return 1;
    return 0;
}

double recomp_patch_var(const char *name)
{
    int i, n = s_nvar;
    for (i = 0; i < n; i++)
        if (s_var[i].name == name || !strcmp(s_var[i].name, name))
            return s_var[i].fn ? s_var[i].fn() : s_var[i].value;
    return 0.0;
}

/* Marks: written before the guest runs, as the settings are. A mark that
 * finds the table full is said on stderr, as a name is. */
#define MARK_MAX 64
static struct {
    const char *name;
    void (*fn)(void);
} s_mark[MARK_MAX];
static volatile int s_nmark;

void recomp_patch_on_mark(const char *name, void (*fn)(void))
{
    if (s_nmark == MARK_MAX) {
        fprintf(stderr, "[PATCH] mark table full (%d): %s is not registered\n", MARK_MAX, name);
        return;
    }
    s_mark[s_nmark].name = name;
    s_mark[s_nmark].fn = fn;
    s_nmark = s_nmark + 1;
}

void recomp_patch_mark(const char *name)
{
    int i, n = s_nmark;
    for (i = 0; i < n; i++)
        if (s_mark[i].name == name || !strcmp(s_mark[i].name, name)) {
            s_mark[i].fn();
            return;
        }
}

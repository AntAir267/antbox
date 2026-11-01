/*
 * xr_log.c - the switch of the print gate; see xr_log.h.
 */
#include "xr_log.h"

#include <stdlib.h>
#include <string.h>

static const char *s_source = "not initialised (everything is printed)";
static int s_forced;
static int s_default = 1;

static int env_set(const char *name)
{
    const char *e = getenv(name);
    return e && *e;
}

void xr_log_init(int release_quiet, int headless)
{
    /* The developer switches that mean "I am reading the log": a traced or
     * scripted run of a release build prints as a developer build does. */
    static const char *const implies[] = {
        "RECOMP_TRACE", "RECOMP_DEBUG", "RECOMP_STDIO_LOG", "RECOMP_FLIP_MARK", NULL
    };
    const char *e = getenv("RECOMP_LOG_DIAG");
    int i;

    s_default = release_quiet ? 0 : 1;
    s_forced = 0;
    g_xr_log_diag = s_default;
    s_source = "build default";
    if (e && (e[0] == '0' || e[0] == '1') && !e[1]) {
        g_xr_log_diag = e[0] == '1';
        s_forced = 1;
        s_source = "RECOMP_LOG_DIAG";
        return;
    }
    for (i = 0; implies[i]; i++) {
        if (env_set(implies[i])) {
            g_xr_log_diag = 1;
            s_forced = 1;
            s_source = implies[i];
            return;
        }
    }
    e = getenv("RECOMP_HEADLESS");
    if (headless || (e && *e && strcmp(e, "0"))) {
        g_xr_log_diag = 1;
        s_forced = 1;
        s_source = headless ? "--headless" : "RECOMP_HEADLESS";
    }
}

int xr_log_set_diag(int on)
{
    if (!s_forced) {
        g_xr_log_diag = on ? 1 : 0;
        s_source = "setting";
    }
    return g_xr_log_diag;
}

int xr_log_default(void)
{
    return s_default;
}

int xr_log_forced(void)
{
    return s_forced;
}

const char *xr_log_source(void)
{
    return s_source;
}

void xr_log_announce(void)
{
    if (g_xr_log_diag)
        XR_NOTICE("[LOG] diagnostic logging is on (%s)\n", s_source);
    else
        XR_NOTICE("[LOG] diagnostic logging is off: errors and the start-up facts only (%s)."
                  " Advanced > Diagnostic logging in the overlay, or RECOMP_LOG_DIAG=1,"
                  " writes the full log\n", s_source);
}

/*
 * xbox_settings_report.c - the settings' part of the bug report
 * (xbox_report.h): settings.toml as it is, every setting with where its
 * value comes from, and the shader cache's counts and size.
 */
#include "xbox_settings.h"
#include "xbox_report.h"
#include "nv2a_backend_common.h"
#include "nv2a_pcache.h"

#include <stdio.h>
#include <string.h>

static const char *settings_path(void)
{
    return xbox_SettingsPath();
}

static void settings_section(xbox_report_buf *b)
{
    int i;

    xbox_report_printf(b, "file: %s\n", *xbox_SettingsPath() ? xbox_SettingsPath()
                                                              : "none (RECOMP_SETTINGS=none)");
    for (i = 0; i < XS_COUNT; i++) {
        const xs_setting *s = xbox_SettingGet((xs_id)i);
        char v[64], d[64];
        const char *var;
        if (!s->key || !s->key[0])
            continue;                 /* a title slot nothing filled */
        var = s->env != RENV_COUNT ? recomp_env_name(s->env) : "RECOMP_PRESENT_PACING + _SPIN_LEAD";
        xbox_SettingEnvString(s, s->value, v, sizeof v);
        xbox_SettingEnvString(s, s->def, d, sizeof d);
        xbox_report_printf(b, "%-26s = %-10s [%s]  default %s, %s\n", s->key, v,
                           s->locked ? "environment" : s->user ? "file" : "default", d, var);
    }
    if (xbox_SettingsNeedRestart())
        xbox_report_printf(b, "(a setting changed in this run takes effect at the next start)\n");
}

/* BO-208: gaps_<backend>.bin, the keys the render thread had to build
 * itself (pipeline keys, not compiled code): the cache folder's own file,
 * whichever backend wrote it. "" when there is none. */
static const char *gaps_path(void)
{
    static const char *const name[] = { "gaps_vulkan.bin", "gaps_metal.bin", "gaps_d3d11.bin" };
    static char path[640];
    nv2a_pcache_stat st;
    char dir[600];
    int i;

    nv2a_pcache_stat_get(&st);
    if (st.active)
        snprintf(dir, sizeof dir, "%s", st.dir);
    else if (!nv2a_pcache_dir(nv2a_host_opts()->shader_cache, nv2a_pcache_app(), dir, sizeof dir))
        return "";
    for (i = 0; i < 3; i++) {
        FILE *f;
        snprintf(path, sizeof path, "%s/%s", dir, name[i]);
        if ((f = fopen(path, "rb")) != NULL) {
            fclose(f);
            return path;
        }
    }
    return "";
}

static void shader_section(xbox_report_buf *b)
{
    static const char *const kind[3] = { "vertex shaders", "fragment shaders", "pipelines" };
    nv2a_pcache_stat st;
    char dir[600] = "";
    int i;

    nv2a_pcache_stat_get(&st);
    if (st.active) {
        for (i = 0; i < 3; i++)
            xbox_report_printf(b, "%s: %u in the list, %u built ahead by the pre-warm, %u"
                               " compiled when first drawn\n", kind[i], st.listed[i],
                               st.warmed[i], st.ack[i]);
        xbox_report_printf(b, "compiles when first drawn: %.1f ms in all, the longest %.1f ms\n",
                           (double)st.ack_ns / 1e6, st.ack_max_us / 1000.0);
        snprintf(dir, sizeof dir, "%s", st.dir);
    } else {
        xbox_report_printf(b, "no backend with a shader cache is up in this process\n");
        if (!nv2a_pcache_dir(nv2a_host_opts()->shader_cache, nv2a_pcache_app(), dir, sizeof dir))
            dir[0] = 0;
    }
    xbox_report_dir_summary(b, "shader cache", dir);
}

void xbox_SettingsReportRegister(void)
{
    xbox_report_attach("settings.toml", settings_path, 65536, 0);
    xbox_report_attach("gaps.bin", gaps_path, 262144, 0);
    xbox_report_section("settings-effective.txt", "the settings: value, source, default, variable",
                        settings_section);
    xbox_report_section("stats.txt", "shaders and pipelines", shader_section);
}

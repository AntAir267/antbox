/*
 * enhance.c - see enhance.h. Reads the toolkit's keys once and stores them
 * as nv2a_host_opts; the per-frame paths only ever read that struct.
 */
#include "enhance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_cfg.h"
#include "kernel.h"
#include "kernel_pacing.h"
#include "nv2a_backend_common.h"
#include "../platform/xr_log.h"   /* the print gate (also force-included) */

#define LOGP "[ENHANCE] "
#define SCALE_MAX 4

static const char *const s_filters[] = { "nearest", "linear", "integer", NULL };
/* Indexes match XBOX_PACING_SPIN / XBOX_PACING_SLEEP. */
static const char *const s_pacing[] = { "spin", "sleep", NULL };
/* present.aspect; "auto" is resolved below, the rest index
 * enum nv2a_present_aspect + 1. */
static const char *const s_aspects[] = { "auto", "frame", "4:3", "16:9", NULL };
/* present.crop: the whole frame (stock), or the part the title drew in. */
static const char *const s_crops[] = { "off", "clip", NULL };
/* The dashboard's HDTV modes, each with those below it (the dashboard has a
 * box per mode; a set that skips one is not offered here). */
static const char *const s_hdtv[] = { "off", "480p", "720p", "1080i", NULL };
static const ULONG s_hdtv_bits[] = {
    0,
    XC_VIDEO_AV_HDTV_480p,
    XC_VIDEO_AV_HDTV_480p | XC_VIDEO_AV_HDTV_720p,
    XC_VIDEO_AV_HDTV_480p | XC_VIDEO_AV_HDTV_720p | XC_VIDEO_AV_HDTV_1080i,
};

int xbox_enhance_init(const char *root, const char *title)
{
    struct nv2a_host_opts o;
    const char *aspect;
    long long scale;
    int rc, pacing, wide, hdtv, pa, half, crop;
    double lead;
    int lead_auto, pacing_unset;
    ULONG xc_video;

    rc = enhance_cfg_init(root, title) < 0 ? -1 : 0;
    memset(&o, 0, sizeof o);

    /* 1.5 is the one fraction: 960x720 for a 640x480 title, on a backend
     * that sizes in halves (Vulkan); the others draw at 1 then. */
    {
        double f = enhance_cfg_float("render.scale", 1.0);
        if (f > 1.49 && f < 1.51) {
            o.render_scale_half = 1;
            scale = 1;
        } else {
            scale = enhance_cfg_int("render.scale", 1);
        }
    }
    if (scale < 1 || scale > SCALE_MAX) {
        long long c = scale < 1 ? 1 : SCALE_MAX;
        fprintf(stderr, LOGP "render.scale=%lld out of range 1..%d; using %lld\n",
                scale, SCALE_MAX, c);
        scale = c;
    }
    o.render_scale = (unsigned)scale;
    half = o.render_scale_half;
    /* Indexes match enum nv2a_present_filter. */
    o.present_filter = enhance_cfg_choice("present.filter", s_filters, NV2A_PRESENT_NEAREST);
    o.fullscreen = enhance_cfg_bool("present.fullscreen", 0);
    /* How lowered guest spin waits wait (kernel_pacing.h): only titles
     * generated with --spin-waits have any. Set before the guest starts. */
    /* Unset (no variable, no file key, no title default): the power mode
     * the overlay calls this host's default, Balanced (sleep, 8 ms lead)
     * where Maximum is not offered (xbox_settings.c k_stock_hidden: Linux,
     * Windows, Android), else the busy-wait. It was the busy-wait on every
     * host, so a port that named no pacing kept a core spinning while the
     * overlay said Balanced (DS2's main thread at 98% of a core). */
    pacing = enhance_cfg_choice("present.pacing", s_pacing, -1);
    pacing_unset = pacing < 0;
    if (pacing_unset) {
#if defined(__APPLE__)
        pacing = XBOX_PACING_SPIN;
#else
        pacing = XBOX_PACING_SLEEP;
#endif
    }
    xbox_PacingSetMode(pacing);
    /* Milliseconds of the flip's vblank hold spun, not slept, at sleep
     * pacing: what keeps an APU's CPU clock up (kernel_pacing.c). */
    /* "auto": picked at run time from the holds (power mode Auto, BO-175;
     * power_auto.h). */
    {
        /* Looked at without the typed lookup, which would report a number
         * in the file as "should be a string". */
        const char *ls = recomp_env(RENV_PRESENT_SPIN_LEAD);
        if (!ls) {
            const recomp_cfg *c = enhance_cfg_lookup("present.spin_lead");
            ls = c && recomp_cfg_type_of(c, "present.spin_lead") == RCFG_STRING
               ? recomp_cfg_string(c, "present.spin_lead", NULL) : NULL;
        }
        lead_auto = ls && !strcmp(ls, "auto");
    }
    /* Balanced's lead where the pacing is the host's default (POWER_LEAD_MS,
     * xbox_settings.c). */
    lead = lead_auto ? 0.0 : enhance_cfg_float("present.spin_lead",
                                               pacing_unset && pacing == XBOX_PACING_SLEEP ? 8.0 : 0.0);
    if (lead < 0 || lead > 20) {
        fprintf(stderr, LOGP "present.spin_lead=%g out of range 0..20 ms; using 0\n", lead);
        lead = 0;
    }
    if (lead_auto)
        xbox_PacingSetSpinLeadAuto();
    else
        xbox_PacingSetSpinLead((uint64_t)(lead * 1e6));

    /* The dashboard's video settings, as the kernel reports them to the
     * title (XC_VIDEO, HalBootSMCVideoMode; kernel.h). What the title does
     * with them is its own: Burnout 3 renders 16:9 anamorphic into its
     * 640x480 buffer and asks D3D for 480p. Stock is neither. Before the
     * kernel bridge starts, which copies the SMC mode into guest memory. */
    wide = enhance_cfg_bool("display.widescreen", 0);
    hdtv = enhance_cfg_choice("display.hdtv", s_hdtv, 0);
    xc_video = (wide ? XC_VIDEO_AV_WIDESCREEN : 0) | s_hdtv_bits[hdtv];
    xbox_SetVideoSettings(xc_video, hdtv ? SMC_VIDEO_MODE_HDTV : SMC_VIDEO_MODE_SCART);

    /* The window's frame shape. A title in its widescreen mode draws 16:9
     * into the same 4:3-sized buffer (anamorphic; the TV stretched it), so
     * auto shows 16:9 when display.widescreen tells it to, and the frame's
     * own shape otherwise (stock). A title that ignores the setting stays
     * 4:3 with present.aspect = "frame". */
    pa = enhance_cfg_choice("present.aspect", s_aspects, 0);
    o.present_aspect = pa == 0 ? (wide ? NV2A_ASPECT_16_9 : NV2A_ASPECT_FRAME) : pa - 1;
    /* A frame the title draws only part of (an HD picture kept inside the
     * TV-safe area, which a TV's overscan hid): the window shows that part. */
    crop = enhance_cfg_choice("present.crop", s_crops, 0);
    nv2a_present_set_crop(crop);
    /* The dashboard's language (XC_LANGUAGE, kernel_xbox.c): a file's value
     * goes into the table the kernel reads. */
    {
        const char *lang = enhance_cfg_string("system.language", NULL);
        if (lang && *lang) {
            recomp_env_set(RENV_LANGUAGE, lang);
            XR_NOTICE( LOGP "system.language=%s\n", lang);
        }
    }
    /* Texture filtering and anti-aliasing: the renderer and the window's
     * post passes read the table, and the settings overlay writes it. */
    {
        static const struct { const char *key; recomp_env_id id; } g[] = {
            { "graphics.aniso", RENV_GFX_ANISO },
            { "graphics.aa", RENV_GFX_AA },
        };
        size_t i;
        for (i = 0; i < sizeof g / sizeof g[0]; i++) {
            const char *v = enhance_cfg_string(g[i].key, NULL);
            if (v && *v) {
                recomp_env_set(g[i].id, v);
                XR_NOTICE( LOGP "%s=%s\n", g[i].key, v);
            }
        }
    }

    /* Hor+ is a later slice; a file or env that asks for it must not be
     * ignored silently. */
    aspect = enhance_cfg_string("display.aspect", "4:3");
    if (strcmp(aspect, "4:3") != 0) {
        fprintf(stderr, LOGP "display.aspect=%s not implemented yet (hor+ is a"
                " later slice); using 4:3\n", aspect);
        aspect = "4:3";
    }

    /* Where the GPU backends keep their pipeline-key list (nv2a_pcache.h):
     * unset is the per-user cache directory, "none" no disk cache. */
    {
        const char *sc = enhance_cfg_string("render.shader_cache", NULL);
        o.shader_cache = sc ? strdup(sc) : NULL;
    }

    nv2a_host_opts_set(&o);
    XR_NOTICE( LOGP "render.scale=%s%u%s present.filter=%s present.fullscreen=%d"
            " present.pacing=%s (display.aspect=%s)\n", half ? "1.5 (a backend without it: " : "",
            o.render_scale, half ? ")" : "",
            s_filters[o.present_filter], o.fullscreen, s_pacing[pacing], aspect);
    XR_NOTICE( LOGP "display.widescreen=%d display.hdtv=%s (XC_VIDEO=0x%08lX)"
            " present.aspect=%s (%s) present.crop=%s\n", wide, s_hdtv[hdtv],
            (unsigned long)xc_video, s_aspects[pa], s_aspects[o.present_aspect + 1], s_crops[crop]);
    if (lead_auto)
        fprintf(stderr, LOGP "present.spin_lead=auto%s\n",
                pacing == XBOX_PACING_SLEEP ? "" : " (no effect: pacing is spin)");
    else if (lead > 0)
        fprintf(stderr, LOGP "present.spin_lead=%g ms%s\n", lead,
                pacing == XBOX_PACING_SLEEP ? "" : " (no effect: pacing is spin)");
    if (o.shader_cache)
        fprintf(stderr, LOGP "render.shader_cache=%s\n", o.shader_cache);
    /* No enhance_cfg_report_unused() here: the title reads its own keys
     * (fps.mode) after this, and calls it then. */
    return rc;
}

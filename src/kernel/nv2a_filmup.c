/*
 * nv2a_filmup.c - the films upscaled on the GPU: the shared part (setting,
 * rule, cost numbers). See nv2a_filmup.h.
 */
#include "nv2a_filmup.h"
#include "recomp_env.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGP "[FILMUP] "

static atomic_int s_mode = -1;      /* -1: not read yet */
static atomic_int s_supported;

const char *nv2a_filmup_mode_name(int mode)
{
    return mode == NV2A_FILMUP_BEST ? "best" : mode == NV2A_FILMUP_CHEAP ? "cheap" : "off";
}

/* Off unless asked for (antbox): Ant's rule is that enhancements come from
 * rendering, never from AI upscaling, and Best is FSRCNNX, a neural network.
 * takedawn's default was Best (Cheap on ARM). */
int nv2a_filmup_default(void)
{
    return NV2A_FILMUP_OFF;
}

static int from_env(void)
{
    const char *e = recomp_env(RENV_GFX_FILMUP);
    if (e && (!strcmp(e, "off") || !strcmp(e, "0")))
        return NV2A_FILMUP_OFF;
    if (e && (!strcmp(e, "cheap") || !strcmp(e, "1")))
        return NV2A_FILMUP_CHEAP;
    if (e && (!strcmp(e, "best") || !strcmp(e, "2")))
        return NV2A_FILMUP_BEST;
    return nv2a_filmup_default();
}

void nv2a_filmup_set_mode(int mode)
{
    int was;
    if (mode < 0 || mode >= NV2A_FILMUP_MODES)
        mode = NV2A_FILMUP_OFF;
    was = atomic_exchange(&s_mode, mode);
    if (was != mode)
        fprintf(stderr, LOGP "film upscale: %s\n", nv2a_filmup_mode_name(mode));
}

int nv2a_filmup_mode(void)
{
    int m = atomic_load_explicit(&s_mode, memory_order_relaxed);
    if (m < 0) {
        int expect = -1;
        m = from_env();
        if (!atomic_compare_exchange_strong(&s_mode, &expect, m))
            m = expect;
    }
    return m;
}

void nv2a_filmup_set_supported(int on)
{
    atomic_store(&s_supported, on != 0);
}

int nv2a_filmup_supported(void)
{
    return atomic_load(&s_supported);
}

int nv2a_filmup_shape(int linear, int render_target, uint32_t w, uint32_t h)
{
    return linear && !render_target && w == 640 && (h == 480 || h == 352);
}

int nv2a_filmup_changed(uint64_t flip, uint64_t *last_flip, uint8_t *changes)
{
    if (*changes && flip - *last_flip > NV2A_FILMUP_GAP)
        *changes = 0;          /* stood still: a picture, or a film that stopped */
    if (*changes < 2)
        (*changes)++;
    *last_flip = flip;
    return *changes >= 2;
}

/* ── the cost ────────────────────────────────────────────────────────── */

/* The window, written from the completion thread (frame_done) and the
 * render thread (flip): microseconds in atomics, no lock. */
static atomic_uint_fast64_t s_win_us, s_win_n, s_win_max_us, s_total;
static atomic_int s_last_mode;
static double   s_fr_sum, s_fr_max, s_win_s;
static unsigned s_fr_n;
static double   s_idle_s = 1e9;          /* since the last filtered frame */
static uint64_t s_seen_total;

/* The last window's numbers (the overlay reads them). */
static _Atomic double s_out_gpu, s_out_gpu_max, s_out_frame, s_out_frame_max;
static atomic_int s_out_recent;

void nv2a_filmup_frame_done(int mode, double gpu_ms)
{
    uint64_t us = gpu_ms > 0 ? (uint64_t)(gpu_ms * 1000.0 + 0.5) : 0, m;
    atomic_fetch_add(&s_win_us, us);
    atomic_fetch_add(&s_win_n, 1);
    atomic_fetch_add(&s_total, 1);
    atomic_store(&s_last_mode, mode);
    m = atomic_load(&s_win_max_us);
    while (us > m && !atomic_compare_exchange_weak(&s_win_max_us, &m, us))
        ;
}

void nv2a_filmup_flip(uint64_t flip, double frame_ms)
{
    uint64_t total = atomic_load(&s_total);
    static int ab = -1;
    if (ab < 0) {
        const char *e = recomp_env(RENV_FILMUP_AB);
        ab = e ? atoi(e) : 0;
    }
    if (ab > 0)   /* RECOMP_DEBUG=filmup_ab=n: off, cheap, best, n flips each */
        atomic_store(&s_mode, (int)((flip / (uint64_t)ab) % 3u));
    if (frame_ms <= 0 || frame_ms > 1000)
        return;
    if (total != s_seen_total) {
        s_seen_total = total;
        s_idle_s = 0;
    } else {
        s_idle_s += frame_ms / 1000.0;
    }
    atomic_store(&s_out_recent, s_idle_s < 2.0);
    if (s_idle_s >= 2.0 && !s_fr_n)
        return;
    s_fr_sum += frame_ms;
    s_fr_n++;
    if (frame_ms > s_fr_max)
        s_fr_max = frame_ms;
    s_win_s += frame_ms / 1000.0;
    if (s_win_s >= 5.0 || (s_idle_s >= 2.0 && s_fr_n)) {
        uint64_t n = atomic_exchange(&s_win_n, 0), us = atomic_exchange(&s_win_us, 0);
        uint64_t mx = atomic_exchange(&s_win_max_us, 0);
        double g = n ? (double)us / 1000.0 / (double)n : 0;
        double f = s_fr_sum / s_fr_n;
        if (n) {
            atomic_store(&s_out_gpu, g);
            atomic_store(&s_out_gpu_max, (double)mx / 1000.0);
            atomic_store(&s_out_frame, f);
            atomic_store(&s_out_frame_max, s_fr_max);
            fprintf(stderr, LOGP "%s: %llu frames in %.1f s, GPU %.2f ms a frame (max %.2f);"
                    " frame time %.2f ms (max %.2f), flip %llu\n",
                    nv2a_filmup_mode_name(atomic_load(&s_last_mode)), (unsigned long long)n,
                    s_win_s, g, (double)mx / 1000.0, f, s_fr_max, (unsigned long long)flip);
        }
        s_fr_sum = s_fr_max = s_win_s = 0;
        s_fr_n = 0;
    }
}

void nv2a_filmup_stats(struct nv2a_filmup_stats *out)
{
    memset(out, 0, sizeof *out);
    out->frames = atomic_load(&s_total);
    out->mode = atomic_load(&s_last_mode);
    out->gpu_ms = atomic_load(&s_out_gpu);
    out->gpu_max_ms = atomic_load(&s_out_gpu_max);
    out->frame_ms = atomic_load(&s_out_frame);
    out->frame_max_ms = atomic_load(&s_out_frame_max);
    out->recent = atomic_load(&s_out_recent);
}

void nv2a_filmup_reset_for_test(void)
{
    atomic_store(&s_mode, -1);
    atomic_store(&s_supported, 0);
    atomic_store(&s_win_us, 0);
    atomic_store(&s_win_n, 0);
    atomic_store(&s_win_max_us, 0);
    atomic_store(&s_total, 0);
    atomic_store(&s_out_gpu, 0.0);
    atomic_store(&s_out_gpu_max, 0.0);
    atomic_store(&s_out_frame, 0.0);
    atomic_store(&s_out_frame_max, 0.0);
    atomic_store(&s_out_recent, 0);
    s_fr_sum = s_fr_max = s_win_s = 0;
    s_fr_n = 0;
    s_idle_s = 1e9;
    s_seen_total = 0;
}

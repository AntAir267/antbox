/*
 * overlay_sounds.cpp - the title's own menu sounds for the overlay; see
 * overlay_sounds.h.
 */
#include "overlay_sounds.h"
#include "xbox_settings.h"
#include "overlay_gamefont.h"   /* gf_read_verified */
#include "apu.h"                /* mcpx_apu_ui_cue */
#include "apu_bus.h"

#include <SDL.h>
#include <atomic>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#define LOGP "[OVERLAY] "

namespace {

struct Cue {
    char path[1024];
    uint64_t size;
    char sha[65];
    uint32_t off, bytes, rate;
    float gain;                         /* linear */
};

Cue s_cue[XBOX_OVERLAY_SND_COUNT];
char s_bus[64];
std::vector<int16_t> s_pcm[XBOX_OVERLAY_SND_COUNT];     /* 48 kHz mono, set by the thread */
std::atomic<int> s_state{-1};          /* -1 none, 0 reading, 1 ready */
bool s_named;

/* Mono s16 at `rate` to 48 kHz, linear. */
void resample(const uint8_t *src, uint32_t n, uint32_t rate, std::vector<int16_t> &out)
{
    const uint64_t m = (uint64_t)n * 48000u / rate;
    out.resize((size_t)m);
    for (uint64_t i = 0; i < m; i++) {
        const double x = (double)i * rate / 48000.0;
        const uint32_t k = (uint32_t)x;
        const double f = x - k;
        const int16_t a = (int16_t)(src[2 * k] | src[2 * k + 1] << 8);
        const int16_t b = k + 1 < n ? (int16_t)(src[2 * k + 2] | src[2 * k + 3] << 8) : a;
        out[(size_t)i] = (int16_t)lrint(a + (b - a) * f);
    }
}

int load_thread(void *)
{
    const uint32_t t0 = SDL_GetTicks();
    int ok = 0;
    for (int i = 0; i < XBOX_OVERLAY_SND_COUNT; i++) {
        const Cue &c = s_cue[i];
        if (!c.path[0] || !s_pcm[i].empty())
            continue;
        const char *why = NULL;
        uint8_t *file = gf_read_verified(c.path, c.size, c.sha, &why);
        if (!file) {
            fprintf(stderr, LOGP "menu sounds: %s: %s, that sound is silent\n", c.path, why);
            continue;
        }
        /* Every cue of this file, from the one read. */
        for (int j = i; j < XBOX_OVERLAY_SND_COUNT; j++) {
            const Cue &d = s_cue[j];
            if (strcmp(d.path, c.path) || !s_pcm[j].empty())
                continue;
            if ((uint64_t)d.off + d.bytes > c.size || d.bytes < 4 || d.rate < 8000 || d.rate > 96000)
                continue;
            resample(file + d.off, d.bytes / 2, d.rate, s_pcm[j]);
            ok++;
        }
        free(file);
    }
    fprintf(stderr, LOGP "menu sounds: %d of %d ready (%u ms)\n", ok, (int)XBOX_OVERLAY_SND_COUNT,
            (unsigned)(SDL_GetTicks() - t0));
    s_state.store(ok ? 1 : -1);
    return 0;
}

/* Mono s16 samples at `rate` to 48 kHz, linear (the port's own PCM). */
void resample16(const int16_t *src, uint32_t n, uint32_t rate, std::vector<int16_t> &out)
{
    const uint64_t m = (uint64_t)n * 48000u / rate;
    out.resize((size_t)m);
    for (uint64_t i = 0; i < m; i++) {
        const double x = (double)i * rate / 48000.0;
        const uint32_t k = (uint32_t)x;
        const double f = x - k;
        const int16_t a = src[k], b = k + 1 < n ? src[k + 1] : a;
        out[(size_t)i] = (int16_t)lrint(a + (b - a) * f);
    }
}

/* The host mixer's gain on the bus the port named: 1 with the mixer off. */
float bus_gain()
{
    if (!s_bus[0] || !apu_bus_enabled())
        return 1.0f;
    const int b = apu_bus_find(s_bus);
    if (b < 0)
        return 1.0f;
    static apu_bus_config cfg;          /* large: not on the stack */
    apu_bus_get_config(&cfg);
    const apu_bus_params &p = cfg.bus[b];
    if (p.mute || p.gain_db <= APU_BUS_OFF_DB)
        return 0.0f;
    for (int k = 0; k < apu_bus_count() && k < APU_BUS_MAX; k++)
        if (k != b && cfg.bus[k].solo && !p.solo)
            return 0.0f;                /* another bus soloed */
    return powf(10.0f, p.gain_db / 20.0f);
}

} // namespace

extern "C" void xbox_OverlaySetMenuSounds(const xbox_overlay_sound *cues, const char *bus_key)
{
    const char *e = getenv("RECOMP_OVERLAY_MENU_SOUNDS");
    if (s_named || !cues || (e && e[0] == '0'))
        return;
    for (int i = 0; i < XBOX_OVERLAY_SND_COUNT; i++) {
        const xbox_overlay_sound &x = cues[i];
        Cue &c = s_cue[i];
        if (!x.path || !x.path[0] || !x.sha256_hex || strlen(x.sha256_hex) != 64)
            continue;
        snprintf(c.path, sizeof c.path, "%s", x.path);
        for (int k = 0; k < 65; k++)
            c.sha[k] = (char)((x.sha256_hex[k] >= 'A' && x.sha256_hex[k] <= 'F') ? x.sha256_hex[k] + 32
                                                                                  : x.sha256_hex[k]);
        c.size = x.size;
        c.off = x.off;
        c.bytes = x.bytes;
        c.rate = x.rate;
        c.gain = powf(10.0f, x.gain_db / 20.0f);
        s_named = true;
    }
    if (!s_named)
        return;
    snprintf(s_bus, sizeof s_bus, "%s", bus_key ? bus_key : "");
    s_state.store(0);
    SDL_Thread *t = SDL_CreateThread(load_thread, "menusnd", NULL);
    if (t)
        SDL_DetachThread(t);
    else
        load_thread(NULL);
}

extern "C" void xbox_OverlaySetMenuSoundsPCM(const xbox_overlay_pcm_sound *cues, const char *bus_key)
{
    const char *e = getenv("RECOMP_OVERLAY_MENU_SOUNDS");
    int ok = 0;
    if (s_named || !cues || (e && e[0] == '0'))
        return;
    for (int i = 0; i < XBOX_OVERLAY_SND_COUNT; i++) {
        const xbox_overlay_pcm_sound &x = cues[i];
        if (!x.pcm || x.samples < 2 || x.rate < 8000 || x.rate > 96000)
            continue;
        resample16(x.pcm, x.samples, x.rate, s_pcm[i]);
        s_cue[i].gain = powf(10.0f, x.gain_db / 20.0f);
        ok++;
    }
    if (!ok)
        return;
    s_named = true;
    snprintf(s_bus, sizeof s_bus, "%s", bus_key ? bus_key : "");
    fprintf(stderr, LOGP "menu sounds: %d of %d from the port\n", ok, (int)XBOX_OVERLAY_SND_COUNT);
    s_state.store(1);
}

extern "C" int xbox_OverlayMenuSoundsState(unsigned wait_ms)
{
    const uint32_t until = SDL_GetTicks() + wait_ms;
    while (s_state.load() == 0 && (int32_t)(until - SDL_GetTicks()) > 0)
        SDL_Delay(5);
    return s_state.load();
}

bool menusnd_named() { return s_named; }

void menusnd_play(int id)
{
    if (id < 0 || id >= XBOX_OVERLAY_SND_COUNT || s_state.load() != 1 || s_pcm[id].empty()
            || xbox_SettingValue(XS_AUDIO_MENU_SOUNDS) == 0.0)
        return;
    const float g = s_cue[id].gain * bus_gain();
    if (g <= 0.0f)
        return;
    mcpx_apu_ui_cue(s_pcm[id].data(), (uint32_t)s_pcm[id].size(), g);
}

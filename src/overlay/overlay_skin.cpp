/*
 * overlay_skin.cpp - a title's own look for the overlay (overlay_skin.h).
 *
 * The port hands over its pictures once (any thread); they are copied, and
 * the main thread makes them Dear ImGui textures between two frames, as the
 * game's sprites are (overlay_gameart.cpp). Drawn as they are, bilinear,
 * the way the title stretches its own screens: no scaler here.
 */
#include "overlay_skin.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <atomic>
#include <mutex>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace {

struct Pic {
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    float x = 0, y = 0;
};

std::mutex s_mu;
xbox_overlay_skin s_skin;
Pic s_pics[SK_COUNT];
std::atomic<bool> s_set{false};
bool s_attached;
SkinPic s_out[SK_COUNT];
bool s_have[SK_COUNT];

void take(Pic &p, const xbox_overlay_picture &src)
{
    p = Pic();
    if (!src.rgba || !src.w || !src.h || src.w > 4096 || src.h > 4096)
        return;
    p.rgba.assign(src.rgba, src.rgba + (size_t)src.w * src.h * 4);
    p.w = src.w;
    p.h = src.h;
    p.x = src.x;
    p.y = src.y;
}

} // namespace

extern "C" void xbox_OverlaySetSkin(const xbox_overlay_skin *skin)
{
    const char *e = getenv("RECOMP_OVERLAY_SKIN");
    if (!skin || (e && e[0] == '0') || s_set.load())
        return;
    std::lock_guard<std::mutex> g(s_mu);
    s_skin = *skin;
    const xbox_overlay_picture *src[SK_COUNT] = {
        &skin->header, &skin->footer, &skin->title, &skin->row, &skin->arrow_left, &skin->arrow_right,
        &skin->prompt_ok, &skin->prompt_back, &skin->prompt_default,
    };
    for (int i = 0; i < SK_COUNT; i++)
        take(s_pics[i], *src[i]);
    /* The copy holds no pointers into the port's memory. */
    xbox_overlay_picture *dst[SK_COUNT] = {
        &s_skin.header, &s_skin.footer, &s_skin.title, &s_skin.row, &s_skin.arrow_left, &s_skin.arrow_right,
        &s_skin.prompt_ok, &s_skin.prompt_back, &s_skin.prompt_default,
    };
    for (int i = 0; i < SK_COUNT; i++)
        dst[i]->rgba = NULL;
    s_set.store(true);
}

bool skin_attach()
{
    if (s_attached)
        return true;
    if (!s_set.load())
        return false;
    std::lock_guard<std::mutex> g(s_mu);
    for (int i = 0; i < SK_COUNT; i++) {
        Pic &p = s_pics[i];
        s_have[i] = !p.rgba.empty();
        if (!s_have[i])
            continue;
        ImTextureData *t = IM_NEW(ImTextureData)();
        t->Create(ImTextureFormat_RGBA32, (int)p.w, (int)p.h);
        memcpy(t->GetPixels(), p.rgba.data(), p.rgba.size());
        t->UseColors = true;
        ImGui::RegisterUserTexture(t);
        s_out[i] = { t->GetTexRef(), (float)p.w, (float)p.h, p.x, p.y };
        p.rgba.clear();
        p.rgba.shrink_to_fit();
    }
    s_attached = true;
    return true;
}

const xbox_overlay_skin *skin() { return &s_skin; }

const SkinPic *skin_pic(int which)
{
    return s_attached && which >= 0 && which < SK_COUNT && s_have[which] ? &s_out[which] : nullptr;
}

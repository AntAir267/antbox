/*
 * overlay_skin.h - a title's own look for the overlay (xbox_settings.h,
 * xbox_OverlaySetSkin): its colours, frame pictures and prompts. Inside the
 * overlay only.
 */
#ifndef OVERLAY_SKIN_H
#define OVERLAY_SKIN_H

#include "xbox_settings.h"
#include "imgui.h"

enum {
    SK_HEADER, SK_FOOTER, SK_TITLE, SK_ROW, SK_ARROW_L, SK_ARROW_R,
    SK_OK, SK_BACK, SK_DEFAULT,
    SK_COUNT
};

struct SkinPic {
    ImTextureRef tex;
    float w, h, x, y;               /* page texels */
};

/* Main thread, between frames: true when a skin is set, its pictures handed
 * to Dear ImGui (once). False without one, or with RECOMP_OVERLAY_SKIN=0. */
bool skin_attach();
/* The skin's numbers and colours (valid while skin_attach() is true). */
const xbox_overlay_skin *skin();
/* A picture, or NULL when the skin has none there. */
const SkinPic *skin_pic(int which);

#endif

/*
 * pad_glyphs.c - which button prompts go with the pad in use.
 * See pad_glyphs.h.
 */
#include "pad_glyphs.h"
#include "input_map.h"
#include "recomp_env.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static const char *const k_mode[XBOX_GLYPHS_MODE_COUNT] = {
    "auto", "xbox", "xbox_modern", "playstation", "playstation5", "nintendo", "steam", "positional", "keyboard"
};
static const char *const k_set[XBOX_GLYPH_SET_COUNT] = {
    "xbox (the title's own)", "playstation", "nintendo (by position)",
    "nintendo (by letter)", "steam", "xbox modern (LB RB LT RT)",
    "positional", "positional (read by letter)", "playstation 5 (DualSense)",
    "keyboard", "xbox (read by letter)", "playstation (read by letter)",
    "playstation 5 (read by letter)", "steam (read by letter)",
    "xbox modern (read by letter)"
};

static int ci_has(const char *s, const char *sub)
{
    size_t n = strlen(sub);
    for (; s && *s; s++) {
        size_t i;
        for (i = 0; i < n; i++) {
            int a = s[i], b = sub[i];
            if (a >= 'A' && a <= 'Z')
                a += 32;
            if (a != b)
                break;
        }
        if (i == n)
            return 1;
    }
    return 0;
}

xbox_pad_family xbox_PadFamilyFromIds(xbox_pad_family by_type, unsigned vendor,
                                      unsigned product, const char *name,
                                      int steam_deck)
{
    if (vendor == 0x28DEu) {
        if (product != 0x11FFu)     /* a Steam Controller, the Deck's own controls */
            return XBOX_PAD_FAMILY_STEAM;
        if (steam_deck)             /* Steam Input's virtual pad, on a Deck */
            return XBOX_PAD_FAMILY_STEAM;
        /* The virtual pad elsewhere: any pad at all, wrapped by Steam and
         * handed on as an Xbox 360 pad (LB RB LT RT, no white or black), so
         * its prompts are the modern Xbox ones whether or not the host typed
         * it (Windows shows it through XInput; a host that types it nothing
         * used to get the original pad's prompts). */
        return XBOX_PAD_FAMILY_XBOX_MODERN;
    }
    if (ci_has(name, "steam deck"))
        return XBOX_PAD_FAMILY_STEAM;
    if (vendor == 0x054Cu && (product == 0x0CE6u || product == 0x0DF2u))
        return XBOX_PAD_FAMILY_PLAYSTATION5;
    if (by_type != XBOX_PAD_FAMILY_XBOX)
        return by_type;
    /* An Xbox 360 or later pad the host has no type for (an older SDL, some
     * receivers and clones): Microsoft's own ids, but not the original
     * Xbox pads (Duke, S), which keep the original prompts; or a name that
     * says so. */
    if (vendor == 0x045Eu && product != 0x0202u && product != 0x0285u &&
        product != 0x0287u && product != 0x0288u && product != 0x0289u)
        return XBOX_PAD_FAMILY_XBOX_MODERN;
    if (ci_has(name, "xbox 360") || ci_has(name, "x-box 360") || ci_has(name, "xinput") ||
        ci_has(name, "xbox one") || ci_has(name, "xbox series") || ci_has(name, "xbox wireless"))
        return XBOX_PAD_FAMILY_XBOX_MODERN;
    if (vendor == 0x054Cu)
        return XBOX_PAD_FAMILY_PLAYSTATION;
    if (vendor == 0x057Eu)
        return XBOX_PAD_FAMILY_NINTENDO;
    return XBOX_PAD_FAMILY_XBOX;
}

/* The table of what each set draws, and why, is in pad_glyphs.h. */
xbox_glyph_set xbox_GlyphSetResolve(xbox_glyph_mode mode, xbox_pad_family family,
                                    int label_mapped, int face_nintendo)
{
    int nintendo_pad = family == XBOX_PAD_FAMILY_NINTENDO;
    /* The title's A is on the right-hand button when the pad is read by
     * letter: SDL maps a Nintendo pad by letter, and the Nintendo face layout
     * swaps it back (or, on any other pad, away from the bottom one). */
    int by_letter = ((nintendo_pad && label_mapped) ? 1 : 0) ^ (face_nintendo ? 1 : 0);

    switch (mode) {
    case XBOX_GLYPHS_XBOX:
        return by_letter ? XBOX_GLYPH_SET_XBOX_SWAPPED : XBOX_GLYPH_SET_XBOX;
    case XBOX_GLYPHS_XBOX_MODERN:
        return by_letter ? XBOX_GLYPH_SET_XBOX_MODERN_SWAPPED : XBOX_GLYPH_SET_XBOX_MODERN;
    case XBOX_GLYPHS_PLAYSTATION:
        return by_letter ? XBOX_GLYPH_SET_PLAYSTATION_SWAPPED : XBOX_GLYPH_SET_PLAYSTATION;
    case XBOX_GLYPHS_PLAYSTATION5:
        return by_letter ? XBOX_GLYPH_SET_PLAYSTATION5_SWAPPED : XBOX_GLYPH_SET_PLAYSTATION5;
    case XBOX_GLYPHS_STEAM:
        return by_letter ? XBOX_GLYPH_SET_STEAM_SWAPPED : XBOX_GLYPH_SET_STEAM;
    case XBOX_GLYPHS_KEYBOARD:
        return XBOX_GLYPH_SET_KEYBOARD;
    case XBOX_GLYPHS_POSITIONAL:
        return by_letter ? XBOX_GLYPH_SET_POSITIONAL_SWAPPED : XBOX_GLYPH_SET_POSITIONAL;
    case XBOX_GLYPHS_NINTENDO:
        return by_letter ? XBOX_GLYPH_SET_NINTENDO_LABEL : XBOX_GLYPH_SET_NINTENDO_POSITION;
    default:
        break;
    }
    switch (family) {
    case XBOX_PAD_FAMILY_PLAYSTATION:
        return by_letter ? XBOX_GLYPH_SET_PLAYSTATION_SWAPPED : XBOX_GLYPH_SET_PLAYSTATION;
    case XBOX_PAD_FAMILY_PLAYSTATION5:
        return by_letter ? XBOX_GLYPH_SET_PLAYSTATION5_SWAPPED : XBOX_GLYPH_SET_PLAYSTATION5;
    case XBOX_PAD_FAMILY_STEAM:
        return by_letter ? XBOX_GLYPH_SET_STEAM_SWAPPED : XBOX_GLYPH_SET_STEAM;
    case XBOX_PAD_FAMILY_NINTENDO:
        return by_letter ? XBOX_GLYPH_SET_NINTENDO_LABEL : XBOX_GLYPH_SET_NINTENDO_POSITION;
    case XBOX_PAD_FAMILY_XBOX_MODERN:
        return face_nintendo ? XBOX_GLYPH_SET_NINTENDO_LABEL : XBOX_GLYPH_SET_XBOX_MODERN;
    case XBOX_PAD_FAMILY_KEYBOARD:  /* the keyboard was used last */
        return XBOX_GLYPH_SET_KEYBOARD;
    case XBOX_PAD_FAMILY_XBOX:      /* a pad of no known family */
        return by_letter ? XBOX_GLYPH_SET_POSITIONAL_SWAPPED : XBOX_GLYPH_SET_POSITIONAL;
    default:                        /* no pad yet */
        return face_nintendo ? XBOX_GLYPH_SET_NINTENDO_LABEL : XBOX_GLYPH_SET_XBOX;
    }
}

/* The words for what each set draws on the title's A B X Y (the table in
 * pad_glyphs.h): one row per set, in xbox_glyph_set order. */
static const char *const k_button_name[XBOX_GLYPH_SET_COUNT][4] = {
    /* XBOX                 */ { "A", "B", "X", "Y" },
    /* PLAYSTATION          */ { "Cross", "Circle", "Square", "Triangle" },
    /* NINTENDO_POSITION    */ { "B", "A", "Y", "X" },
    /* NINTENDO_LABEL       */ { "A", "B", "X", "Y" },
    /* STEAM                */ { "A", "B", "X", "Y" },
    /* XBOX_MODERN          */ { "A", "B", "X", "Y" },
    /* POSITIONAL           */ { "bottom button", "right button", "left button", "top button" },
    /* POSITIONAL_SWAPPED   */ { "right button", "bottom button", "top button", "left button" },
    /* PLAYSTATION5         */ { "Cross", "Circle", "Square", "Triangle" },
    /* KEYBOARD             */ { "Space", "Backspace", "Shift", "Ctrl" },
    /* XBOX_SWAPPED         */ { "B", "A", "Y", "X" },
    /* PLAYSTATION_SWAPPED  */ { "Circle", "Cross", "Triangle", "Square" },
    /* PLAYSTATION5_SWAPPED */ { "Circle", "Cross", "Triangle", "Square" },
    /* STEAM_SWAPPED        */ { "B", "A", "Y", "X" },
    /* XBOX_MODERN_SWAPPED  */ { "B", "A", "Y", "X" },
};

const char *xbox_GlyphButtonName(xbox_glyph_set set, int action)
{
    if ((unsigned)set >= XBOX_GLYPH_SET_COUNT || (unsigned)action > 3u)
        return "";
    return k_button_name[set][action];
}

const char *xbox_GlyphModeName(xbox_glyph_mode m)
{
    return (unsigned)m < XBOX_GLYPHS_MODE_COUNT ? k_mode[m] : "auto";
}

int xbox_GlyphModeFromName(const char *s)
{
    int i;
    if (!s)
        return -1;
    for (i = 0; i < XBOX_GLYPHS_MODE_COUNT; i++)
        if (ci_has(s, k_mode[i]) && strlen(s) == strlen(k_mode[i]))
            return i;
    if (ci_has(s, "deck") && strlen(s) == 4)      /* the family's first name */
        return XBOX_GLYPHS_STEAM;
    return -1;
}

/* ── state ───────────────────────────────────────────────────────────── */

static _Atomic int s_mode = -1;                 /* -1: the environment */
static _Atomic int s_family = XBOX_PAD_FAMILY_NONE;
static _Atomic int s_label_mapped;
static _Atomic int s_set = XBOX_GLYPH_SET_XBOX;
static void (*_Atomic s_listener)(int);

xbox_glyph_mode xbox_InputGlyphMode(void)
{
    int m = atomic_load_explicit(&s_mode, memory_order_relaxed);
    if (m < 0) {
        m = xbox_GlyphModeFromName(recomp_env(RENV_INPUT_GLYPHS));
        if (m < 0)
            m = XBOX_GLYPHS_AUTO;
    }
    return (xbox_glyph_mode)m;
}

void xbox_InputGlyphsRefresh(void)
{
    int set = xbox_GlyphSetResolve(xbox_InputGlyphMode(),
                                   (xbox_pad_family)atomic_load_explicit(&s_family, memory_order_relaxed),
                                   atomic_load_explicit(&s_label_mapped, memory_order_relaxed),
                                   xbox_InputFaceLayoutNintendo());
    void (*fn)(int);
    if (atomic_exchange_explicit(&s_set, set, memory_order_relaxed) == set)
        return;
    fprintf(stderr, "[INPUT] button prompts: %s\n", k_set[set]);
    fn = atomic_load_explicit(&s_listener, memory_order_relaxed);
    if (fn)
        fn(set);
}

void xbox_InputSetGlyphMode(int mode)
{
    if (mode >= XBOX_GLYPHS_MODE_COUNT)
        mode = XBOX_GLYPHS_AUTO;
    atomic_store_explicit(&s_mode, mode < 0 ? -1 : mode, memory_order_relaxed);
    xbox_InputGlyphsRefresh();
}

void xbox_InputSetActivePad(xbox_pad_family family, int label_mapped)
{
    atomic_store_explicit(&s_family, (int)family, memory_order_relaxed);
    atomic_store_explicit(&s_label_mapped, label_mapped != 0, memory_order_relaxed);
    xbox_InputGlyphsRefresh();
}

xbox_pad_family xbox_InputActivePadFamily(void)
{
    return (xbox_pad_family)atomic_load_explicit(&s_family, memory_order_relaxed);
}

xbox_glyph_set xbox_InputGlyphSet(void)
{
    return (xbox_glyph_set)atomic_load_explicit(&s_set, memory_order_relaxed);
}

void xbox_InputSetGlyphListener(void (*fn)(int set))
{
    atomic_store_explicit(&s_listener, fn, memory_order_relaxed);
    xbox_InputGlyphsRefresh();
    if (fn)
        fn((int)xbox_InputGlyphSet());
}

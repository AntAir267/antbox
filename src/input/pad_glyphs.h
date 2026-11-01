/*
 * pad_glyphs.h - which button prompts go with the pad in use.
 *
 * A title draws its prompts for the Xbox pad: A B X Y discs, L and R
 * triggers, white and black buttons, START. input.glyphs (the overlay's Input
 * page, RECOMP_INPUT_GLYPHS) chooses whose pad they are drawn for instead:
 *
 *   auto         the family of the pad that last pressed a button; a pad of
 *                an unknown family gets the positional set
 *   xbox         the title's own prompts, nothing replaced (the original
 *                pad: white and black buttons, L and R triggers)
 *   xbox_modern  A B X Y as they are, LB RB LT RT and the Menu button: an
 *                Xbox 360 / One / Series pad, which has no white or black
 *   playstation  cross / circle / square / triangle (coloured, as on the
 *                DualShock 3 and 4), L1 R1 L2 R2
 *   playstation5 the same four symbols as a DualSense prints them: light
 *                grey, no colours
 *   nintendo     A B X Y as a Nintendo pad prints them, L R ZL ZR
 *   steam        Valve's: the Steam Deck's own controls and the Steam
 *                Controllers. A B X Y as they are, L1 R1 L2 R2, Menu
 *   positional   any pad: no letters on the face buttons, a diamond of four
 *                dots with the one to press filled (bottom, right, left,
 *                top); L1 R1 L2 R2, Menu. auto picks it for
 *                a pad of unknown family (a handheld's built-in pad)
 *   keyboard     key caps for the keyboard's own keys (keyboard.h): Space for
 *                A, Backspace B, Shift X, Ctrl Y, Q and E for the white and
 *                black buttons, S and W for the triggers, Enter for Start.
 *                auto picks it while the keyboard was the last device used
 *
 * This file is the choice, without SDL and without a renderer: the family of
 * a pad from what the host knows about it, and the glyph set that follows
 * from the setting, the family and the face-button layout. The pictures and
 * the textures they go into are the title's (nv2a_texrep.h); it hears of a
 * change through xbox_InputSetGlyphListener.
 */
#ifndef XBOX_PAD_GLYPHS_H
#define XBOX_PAD_GLYPHS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XBOX_PAD_FAMILY_XBOX,          /* a pad nothing is known about (an unknown family) */
    XBOX_PAD_FAMILY_XBOX_MODERN,   /* an Xbox 360 / One / Series pad (XInput) */
    XBOX_PAD_FAMILY_PLAYSTATION,
    XBOX_PAD_FAMILY_NINTENDO,
    XBOX_PAD_FAMILY_STEAM,
    XBOX_PAD_FAMILY_PLAYSTATION5,  /* a DualSense / DualSense Edge */
    XBOX_PAD_FAMILY_NONE,          /* no pad has been used (the state before the first button): the title's own */
    XBOX_PAD_FAMILY_KEYBOARD,      /* the keyboard (keyboard.c) was the last input device: not a pad, but the active "pad" for the prompts */
    XBOX_PAD_FAMILY_COUNT
} xbox_pad_family;

/* input.glyphs, in the order of the setting's choices. */
typedef enum {
    XBOX_GLYPHS_AUTO,
    XBOX_GLYPHS_XBOX,
    XBOX_GLYPHS_XBOX_MODERN,
    XBOX_GLYPHS_PLAYSTATION,
    XBOX_GLYPHS_PLAYSTATION5,
    XBOX_GLYPHS_NINTENDO,
    XBOX_GLYPHS_STEAM,
    XBOX_GLYPHS_POSITIONAL,
    XBOX_GLYPHS_KEYBOARD,
    XBOX_GLYPHS_MODE_COUNT
} xbox_glyph_mode;

/* What is drawn. 0 is the title's own. Nintendo is two sets, because the
 * same four letters sit in other places on its pads:
 *   POSITION  the title's A is the bottom button, which a Nintendo pad
 *             prints B: the prompt for A shows B, for B A, for X Y, for Y X
 *   LABEL     the title's A is the button printed A: the letters stay, only
 *             the look changes
 * Positional is two sets for the same reason, and names the place of the
 * button instead of its letter:
 *   POSITIONAL          the title's A is the bottom button, B the right one,
 *                       X the left one, Y the top one
 *   POSITIONAL_SWAPPED  the pad is read by letter: A is the right button, B the
 *                       bottom one, X the top one, Y the left one
 * and so is every other set that prints something on the face buttons. A
 * *_SWAPPED set is the same pictures with the title's A and B, and X and Y,
 * exchanged: the title's A is the right-hand button, which an Xbox or Valve
 * pad prints B and a PlayStation pad draws as the circle:
 *   XBOX_SWAPPED, XBOX_MODERN_SWAPPED, STEAM_SWAPPED     B A Y X
 *   PLAYSTATION_SWAPPED, PLAYSTATION5_SWAPPED    circle cross triangle square
 * (the order is the title's A B X Y; xbox_GlyphSetResolve has the whole table).
 * The keyboard has no swapped set: its keys are named, not placed. */
typedef enum {
    XBOX_GLYPH_SET_XBOX,
    XBOX_GLYPH_SET_PLAYSTATION,
    XBOX_GLYPH_SET_NINTENDO_POSITION,
    XBOX_GLYPH_SET_NINTENDO_LABEL,
    XBOX_GLYPH_SET_STEAM,
    XBOX_GLYPH_SET_XBOX_MODERN,
    XBOX_GLYPH_SET_POSITIONAL,
    XBOX_GLYPH_SET_POSITIONAL_SWAPPED,
    XBOX_GLYPH_SET_PLAYSTATION5,
    XBOX_GLYPH_SET_KEYBOARD,
    XBOX_GLYPH_SET_XBOX_SWAPPED,
    XBOX_GLYPH_SET_PLAYSTATION_SWAPPED,
    XBOX_GLYPH_SET_PLAYSTATION5_SWAPPED,
    XBOX_GLYPH_SET_STEAM_SWAPPED,
    XBOX_GLYPH_SET_XBOX_MODERN_SWAPPED,
    XBOX_GLYPH_SET_COUNT
} xbox_glyph_set;

/* The family of a pad. by_type: what the host's own classification says
 * (SDL_GameControllerGetType): XBOX_MODERN for its Xbox 360 and Xbox One
 * types, XBOX when it says nothing. The USB ids and the name settle the rest:
 *
 *   Valve (28DE), any product but 11FF: STEAM. SDL2 has no type for these
 *     (sdl2-compat reports SDL3's SDL_GAMEPAD_TYPE_STEAM as unknown); the
 *     products SDL 3.4 lists are the Steam Controllers 1101 1102 1105 1106
 *     1142 1201 1202, the Steam Deck's own controls 1205, and the 2026 Steam
 *     Controller 1302 (cable), 1303 (Bluetooth), 1304 and 1305 (its dongles)
 *   28DE:11FF, Steam Input's virtual pad: every pad Steam wraps arrives as
 *     this one, whatever it is, so it says nothing about the pad in the
 *     hands. STEAM only when steam_deck says the machine is a Steam Deck (the
 *     SteamDeck=1 Steam sets there: its own controls are what is most likely
 *     held); elsewhere XBOX_MODERN: Steam hands every wrapped pad on as an
 *     Xbox 360 pad
 *   Sony 054C:0CE6 (DualSense) and 054C:0DF2 (DualSense Edge): PLAYSTATION5,
 *     whatever type the host gave (an SDL that predates the type says PS4)
 *   Sony (054C) and Nintendo (057E) pads the host has no type for
 *   Microsoft (045E) pads the host has no type for, but the original Xbox
 *     pads (0202 0285 0287 0288 0289), and names with "Xbox 360", "XInput",
 *     "Xbox One", "Xbox Series" or "Xbox Wireless" in them: XBOX_MODERN
 *   a name with "Steam Deck" in it: STEAM
 *
 * name may be NULL. */
xbox_pad_family xbox_PadFamilyFromIds(xbox_pad_family by_type, unsigned vendor,
                                      unsigned product, const char *name,
                                      int steam_deck);

/* The set for a setting, the active pad and the face layout. Pure.
 *   label_mapped   the host already maps this pad's face buttons by their
 *                  printed letters (SDL does for a Nintendo pad: see below)
 *   face_nintendo  input.face_layout = nintendo (A/B and X/Y change places
 *                  as the pad is read, input_map.c xbox_InputMirrorFace*)
 *
 * THE RULE: a prompt draws the physical button that really triggers its
 * action, after the host's mapping and our face-layout swap, as the pad the
 * set stands for prints that button.
 *
 * Which button is the title's A? Name the buttons S (bottom), E (right),
 * W (left), N (top). SDL reports its A on S, B on E, X on W, Y on N, except
 * for a pad it maps by printed letter (label_mapped), where its A is on E,
 * B on S, X on N, Y on W. The Nintendo face layout then swaps A with B and
 * X with Y. So the title's A, B, X, Y are
 *   on S E W N   ("by position"): label_mapped and face_nintendo both off or both on
 *   on E S N W   ("by letter"):   exactly one of them on
 * (label_mapped only counts for a Nintendo pad; the keyboard's keys are
 * mapped to the Xbox layout and never swapped.) What each set draws for the
 * title's A B X Y:
 *
 *   set                      prints on S E W N       by position     by letter
 *   XBOX, XBOX_MODERN, STEAM A  B  X  Y              A B X Y         B A Y X
 *   PLAYSTATION, PS5         cross circle square tri cross circle    circle cross
 *                                                    square tri      tri square
 *   NINTENDO                 B  A  Y  X              B A Y X         A B X Y
 *   POSITIONAL               a dot on S, E, W, N     S E W N         E S N W
 *   KEYBOARD                 Space Back Shift Ctrl   the same keys whatever the layout
 *
 * The by-letter column is the *_SWAPPED sets (XBOX_SWAPPED,
 * XBOX_MODERN_SWAPPED, STEAM_SWAPPED, PLAYSTATION_SWAPPED,
 * PLAYSTATION5_SWAPPED, POSITIONAL_SWAPPED) and NINTENDO_LABEL; the
 * by-position column is the unmarked set and NINTENDO_POSITION. A fixed
 * choice (input.glyphs = ...) is the set it names, swapped or not by the
 * rule. Example, a Retroid Pocket (built-in pad, A printed on the right)
 * with Input > Face buttons = Nintendo: the title's A is on E; Positional
 * fills the right dot, Nintendo draws "A", PlayStation the circle.
 *
 * auto: the pad's family picks the set and the same question swaps it
 * (PlayStation, PS5, Steam, Nintendo; an unknown pad is POSITIONAL). Two
 * cases follow the player's word instead of the pad: an Xbox 360 / One /
 * Series pad, or no pad at all, with face_nintendo on is NINTENDO_LABEL (the
 * player turned the Nintendo layout on because the pad is printed the Nintendo
 * way, so the button on E is printed A). auto with a pad nothing is known
 * about (FAMILY_XBOX: an SDL pad of no known type or vendor, a handheld's
 * built-in pad) is POSITIONAL: no letters to be wrong. auto with no pad at all
 * (FAMILY_NONE) is XBOX: the title's own prompts, nothing replaced. auto with
 * the keyboard as the last device (FAMILY_KEYBOARD) is KEYBOARD, whatever
 * the face layout. The next button of a pad puts the pad's set back.
 *
 * What SDL does in our version (sdl2-compat 2.32.74 over SDL 3, the SDL2 we
 * build with on macOS, Windows and Linux; read in its sdl2_compat.c):
 * SDL_GameControllerGetButton swaps A/B and X/Y for a pad whose SDL 3 label
 * for the south button is B (Switch Pro, Joy-Cons, ...), while the hint
 * SDL_GAMECONTROLLER_USE_BUTTON_LABELS is true (the default; looked at when
 * the pad is opened and when the hint changes). SDL 2.0.12 and later does the
 * same for its Nintendo types, with the same default. That is label_mapped:
 * xinput_device.c sets it from the SDL controller type (the Nintendo ones) and
 * the same hint, so a Switch pad on a desktop reads by letter and the
 * built-in pad of a handheld, which SDL does not type Nintendo (the Retroid
 * Pocket's is "unknown"), by position. The Windows XInput path has no labels. */
xbox_glyph_set xbox_GlyphSetResolve(xbox_glyph_mode mode, xbox_pad_family family,
                                    int label_mapped, int face_nintendo);

/* The title's A (0), B (1), X (2) or Y (3) as the words of a sentence, for a
 * set, naming the button that does it by the same table (the overlay's
 * hints): "A", "Cross", "bottom button"... The keyboard set names its keys
 * (Space, Backspace, Shift, Ctrl). "" for an action or set out of range. */
const char *xbox_GlyphButtonName(xbox_glyph_set set, int action);

/* input.glyphs as text and back ("auto", "xbox", ...); -1: not one. */
const char *xbox_GlyphModeName(xbox_glyph_mode m);
int         xbox_GlyphModeFromName(const char *s);

/* ── state ───────────────────────────────────────────────────────────── */

/* The setting. -1: back to RECOMP_INPUT_GLYPHS (auto when unset). */
void            xbox_InputSetGlyphMode(int mode);
xbox_glyph_mode xbox_InputGlyphMode(void);
/* The pad that last pressed a button (a host backend calls it). */
void            xbox_InputSetActivePad(xbox_pad_family family, int label_mapped);
xbox_pad_family xbox_InputActivePadFamily(void);
/* Resolve again (after the face layout changed) and tell the listener if
 * the set is another one. */
void            xbox_InputGlyphsRefresh(void);
xbox_glyph_set  xbox_InputGlyphSet(void);
/* fn(set) at once with the set in use, and on every change after. From the
 * thread that makes the change (the window's, the overlay's): fn must be
 * safe there. NULL: none. */
void xbox_InputSetGlyphListener(void (*fn)(int set));

#ifdef __cplusplus
}
#endif

#endif /* XBOX_PAD_GLYPHS_H */

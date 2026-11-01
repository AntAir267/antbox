/*
 * overlay_mixer.inc -- the Mixer page of the settings overlay (BO-114):
 * the buses of src/apu/apu_bus.h, live. Included by overlay_ui.cpp (it uses
 * that file's theme, input steps and helpers).
 *
 * Opened from the Audio page. Views: the bus list with the master rows, one
 * bus, the speakers, the ducking slot. Every change is heard at once and
 * written to the followed preset file (mixer.toml) a moment later.
 *
 * A and B: A is "your mix", the file; B is a reference preset held in
 * memory. F7, or Back + Y on a pad, switches what is heard, with the
 * overlay open or closed.
 */

enum { MX_MAIN, MX_BUS, MX_SPEAKERS, MX_DUCK, MX_VIEWS };
enum { MK_DB, MK_MS, MK_BOOL, MK_CHOICE, MK_ACTION, MK_INFO, MK_BUS, MK_UNIT };
enum { MA_ENABLE, MA_PRESET, MA_AB, MA_BREF, MA_SAVE_COPY, MA_SPEAKERS, MA_DUCK,
       MA_RESET_OVERS, MA_BUS_RESET, MA_PAN_MOVE, MA_AUD, MA_AUD_SAVE };

struct MxItem {
    int kind;
    const char *label;
    float *f;                   /* MK_DB, MK_MS */
    int *i;                     /* MK_BOOL, MK_CHOICE (the value itself) */
    float mn, mx, step;
    bool off_below;             /* MK_DB: under mn is "off" */
    float on_db;                /* MK_DB: what A sets from "off" */
    const char *const *names;   /* MK_CHOICE */
    const int *vals;
    int nvals;
    int action;                 /* MK_ACTION */
    int bus;                    /* MK_BUS */
    const char *hint;
    char text[96];              /* MK_ACTION / MK_INFO value text */
};

static int s_mx_view, s_mx_bus, s_mx_sel[MX_VIEWS];
static bool s_mx_scroll;
static apu_bus_config s_mx_b;           /* the reference */
static int s_mx_b_idx;                  /* its preset */
static bool s_mx_b_ready, s_mx_live_b;
static int s_mx_preset_idx;             /* the Preset row's choice */
static bool s_mx_save_pending;
static uint32_t s_mx_save_at;
static apu_bus_config s_mx_save_cfg;
static unsigned s_mx_gen;
static char s_mx_note[240];
static int s_mx_aud_keep;              /* the Audition recording row: seconds */
static bool s_mx_grab;                 /* the pan position follows the D-pad / stick */

/* The presets on offer: the built-in ones, then mixer-*.toml beside the
 * followed file. */
#define MX_USER_MAX 16
static char s_mx_user[MX_USER_MAX][64];
static int s_mx_nuser;

static void mx_dir(char *out, size_t n)
{
    const char *f = apu_bus_file();
    const char *sl = strrchr(f, '/');
#if defined(_WIN32)
    const char *bs = strrchr(f, '\\');
    if (bs > sl) sl = bs;
#endif
    if (sl) snprintf(out, n, "%.*s", (int)(sl - f), f);
    else snprintf(out, n, ".");
}

static void mx_scan_user()
{
    char dir[1024];
    s_mx_nuser = 0;
    mx_dir(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_mx_nuser < MX_USER_MAX) {
        size_t n = strlen(e->d_name);
        if (n > 11 && n < sizeof s_mx_user[0] && !strncmp(e->d_name, "mixer-", 6)
                && !strcmp(e->d_name + n - 5, ".toml"))
            snprintf(s_mx_user[s_mx_nuser++], sizeof s_mx_user[0], "%s", e->d_name);
    }
    closedir(d);
    qsort(s_mx_user, (size_t)s_mx_nuser, sizeof s_mx_user[0],
          [](const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); });
}

static int mx_preset_count() { return apu_bus_preset_count() + s_mx_nuser; }

static const char *mx_preset_name(int i)
{
    int nb = apu_bus_preset_count();
    if (i < nb) return apu_bus_preset_name(i);
    return i - nb < s_mx_nuser ? s_mx_user[i - nb] : "";
}

static bool mx_preset_get(int i, apu_bus_config *c)
{
    int nb = apu_bus_preset_count();
    if (i < nb) return apu_bus_preset_get(i, c) == 0;
    if (i - nb >= s_mx_nuser) return false;
    char dir[1024], path[1200], err[160];
    mx_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s/%s", dir, s_mx_user[i - nb]);
    apu_bus_config_neutral(c);
    return apu_bus_load_file(path, c, err, sizeof err) == 0;
}

static void mx_b_load()
{
    if (!mx_preset_get(s_mx_b_idx, &s_mx_b))
        apu_bus_config_neutral(&s_mx_b);
    s_mx_b_ready = true;
}

/* What is heard: A, the file's mix, or B, the reference. */
static void mx_ab_toggle()
{
    if (!apu_bus_enabled())
        return;
    if (!s_mx_b_ready)
        mx_b_load();
    if (!s_mx_live_b) {
        /* Leaving A: it is what the mixer has now. */
        apu_bus_get_config(&s_mx_save_cfg);
        if (s_mx_save_pending) {
            apu_bus_commit(&s_mx_save_cfg);
            s_mx_save_pending = false;
        }
        apu_bus_set_config(&s_mx_b);
        s_mx_live_b = true;
    } else {
        apu_bus_set_config(&s_mx_save_cfg);
        s_mx_live_b = false;
    }
    s_mx_gen = apu_bus_file_gen();
    fprintf(stderr, LOGP "mixer: listening to %s\n",
            s_mx_live_b ? mx_preset_name(s_mx_b_idx) : "A (your mix)");
}

/* A change from this page: heard now; A is also written to its file, once
 * the dial has been still for a moment. */
static void mx_changed(const apu_bus_config &c)
{
    apu_bus_set_config(&c);
    if (s_mx_live_b) {
        s_mx_b = c;
    } else {
        s_mx_save_cfg = c;
        s_mx_save_pending = true;
        s_mx_save_at = now_ms() + 400;
    }
}

static void mx_poll()
{
    if (s_mx_save_pending && (int32_t)(now_ms() - s_mx_save_at) >= 0) {
        s_mx_save_pending = false;
        if (!s_mx_live_b)
            apu_bus_commit(&s_mx_save_cfg);
    }
    /* The file changed under us (an editor): that is A, and it is heard. */
    if (s_mx_live_b && apu_bus_file_gen() != s_mx_gen) {
        s_mx_live_b = false;
        s_mx_gen = apu_bus_file_gen();
    }
}

static void mx_open()
{
    s_mx_open = true;
    s_mx_view = MX_MAIN;
    s_mx_scroll = true;
    s_mx_note[0] = 0;
    mx_scan_user();
    if (!s_mx_b_ready)
        mx_b_load();
}

/* B / Esc: a view up, then out. Returns false when the page closed. */
static bool mx_back()
{    if (s_mx_grab) {
        s_mx_grab = false;
        return true;
    }

    if (s_mx_view != MX_MAIN) {
        s_mx_view = MX_MAIN;
        s_mx_scroll = true;
        return true;
    }
    s_mx_open = false;
    return false;
}

static const char *const k_mx_route[] = { "The game's panning", "Sends", "A place in the room (pan)" };
static const int k_mx_route_v[] = { APU_BUS_ROUTE_TITLE, APU_BUS_ROUTE_SENDS, APU_BUS_ROUTE_PAN };
static const char *const k_mx_lim[] = { "Off (hard clamp, as shipped)", "Soft clip", "Peak limiter (+1.33 ms)" };
static const int k_mx_lim_v[] = { APU_BUS_LIM_OFF, APU_BUS_LIM_SOFT, APU_BUS_LIM_PEAK };
static const char *const k_mx_hz[] = { "Off", "60 Hz", "80 Hz", "100 Hz", "120 Hz", "150 Hz" };
static const int k_mx_hz_v[] = { 0, 60, 80, 100, 120, 150 };
static const char *const k_mx_keep[] = { "Off", "The last 30 s", "The last 60 s" };
static const int k_mx_keep_v[] = { 0, 30, 60 };

static void mx_aud_path(char *out, size_t n)
{
    char dir[1024];
    mx_dir(dir, sizeof dir);
    snprintf(out, n, "%s/mixer-audition.clip", dir);
}

static const char *const k_mx_xo[] = { "Full range", "60 Hz", "80 Hz", "100 Hz", "120 Hz", "150 Hz" };

static char s_mx_busnames[APU_BUS_MAX][40];
static const char *s_mx_busname_p[APU_BUS_MAX];
static int s_mx_busvals[APU_BUS_MAX];

static int mx_items(apu_bus_config &c, MxItem *it, int max)
{
    int n = 0;
    auto add = [&](int kind, const char *label, const char *hint) -> MxItem & {
        MxItem &m = it[n < max - 1 ? n++ : n];
        m = MxItem{};
        m.kind = kind;
        m.label = label;
        m.hint = hint;
        return m;
    };
    auto db = [&](const char *label, float *f, float mn, float mx, bool off, const char *hint) {
        MxItem &m = add(MK_DB, label, hint);
        m.f = f; m.mn = mn; m.mx = mx; m.step = 0.5f; m.off_below = off;
    };
    auto unit = [&](const char *label, float *f, float mn, float mx, const char *hint) {
        MxItem &m = add(MK_UNIT, label, hint);
        m.f = f; m.mn = mn; m.mx = mx; m.step = 0.05f;
    };
    auto choice = [&](const char *label, int *i, const char *const *names, const int *vals, int nv, const char *hint) {
        MxItem &m = add(MK_CHOICE, label, hint);
        m.i = i; m.names = names; m.vals = vals; m.nvals = nv;
    };
    auto act = [&](int a, const char *label, const char *hint) -> MxItem & {
        MxItem &m = add(MK_ACTION, label, hint);
        m.action = a;
        return m;
    };
    const int nbus = apu_bus_count();
    const bool on = apu_bus_enabled() != 0;

    if (on && s_mx_live_b)
        add(MK_INFO, "YOU HEAR B, the reference preset. Edits here change B and are not saved. F7 / Back+Y: back to A.", NULL);

    if (s_mx_view == MX_MAIN) {
        MxItem &en = act(MA_ENABLE, "Mixer", "Off plays the game's mix untouched. With the Factory preset on, the output is still identical bit for bit; everything below is a change you make.");
        snprintf(en.text, sizeof en.text, "%s", on ? "On" : "Off");
        if (!on) {
            add(MK_INFO, "Turn the mixer on to hear and edit the buses below.", NULL);
            return n;
        }
        add(MK_INFO, "The game keeps running while the mixer is open, so you hear changes as you make them.", NULL);
        MxItem &ab = act(MA_AB, "Listening to", "A is your mix, kept in mixer.toml. B is a reference held in memory. F7, or Back + Y on a pad, switches between them at any time, even with this menu closed.");
        snprintf(ab.text, sizeof ab.text, "%s", s_mx_live_b ? "B (reference)" : "A (your mix)");
        MxItem &br = act(MA_BREF, "B is", "Left / right: the preset B holds. Edits made while B is heard change B only and are not saved.");
        snprintf(br.text, sizeof br.text, "%s", mx_preset_name(s_mx_b_idx));
        MxItem &pr = act(MA_PRESET, "Load a preset into A", "Left / right to choose, A to load it over your mix (mixer.toml is rewritten). Files named mixer-<name>.toml beside it are listed too.");
        snprintf(pr.text, sizeof pr.text, "%s", mx_preset_name(s_mx_preset_idx));
        act(MA_SAVE_COPY, "Save a copy of A as a preset file", "Writes mixer-1.toml (or the next free number) beside mixer.toml. Rename it as you like: mixer-<name>.toml.");
        /* Audition: a recorded stretch of the game's sound, looped through
         * the mixer in place of the live sound. */
        s_mx_aud_keep = apu_bus_audition_kept_seconds();
        choice("Audition recording", &s_mx_aud_keep, k_mx_keep, k_mx_keep_v, 3, "Keeps the last 30 or 60 seconds of the game's sound in memory while you play (about 3 MB per second), so you can loop it below. Off records nothing and holds no memory.");
        {
            apu_bus_audition_info ai;
            apu_bus_audition_get(&ai);
            const int st = apu_bus_audition_state();
            MxItem &au = act(MA_AUD, "Audition", "A loops the recording through the mixer in place of the live sound (the game keeps running, unheard), so every change is heard on the same material. A again returns to the live sound. Right selects the saved clip instead. The loop point is cross-faded over 2 s.");
            if (st == APU_BUS_AUD_OFF)
                snprintf(au.text, sizeof au.text, s_mx_aud_keep ? "Off (live sound), %.0f s recorded" : "Off (live sound)", (double)ai.kept_s);
            else
                snprintf(au.text, sizeof au.text, "%s  %.1f / %.1f s", st == APU_BUS_AUD_FILE ? "Looping the saved clip" : "Looping the recording",
                         (double)ai.position_s, (double)ai.length_s);
            if (st != APU_BUS_AUD_OFF)
                act(MA_AUD_SAVE, "Save this clip", "Saves the looping clip to mixer-audition.clip beside mixer.toml, so you can loop it again after a restart.");
        }
        for (int b = 0; b < nbus; b++) {
            static char bus_hint[32][220];      /* the item keeps the pointer; one per bus */
            snprintf(bus_hint[b & 31], sizeof bus_hint[0], "Left / right: the bus gain, 0.5 dB a step (hold: faster). %s: its route, sends, trims and LFE send. LB mutes, RB solos, %s resets the bus.",
                     act_name(0), act_name(3));
            MxItem &m = add(MK_BUS, apu_bus_name(b), bus_hint[b & 31]);
            m.bus = b;
            m.f = &c.bus[b].gain_db;
            m.mn = -60.0f; m.mx = 12.0f; m.step = 0.5f; m.off_below = true;
        }
        choice("Limiter", &c.limiter, k_mx_lim, k_mx_lim_v, 3, "What happens above the ceiling. Off: samples past full scale are clamped, as shipped. Soft clip: a tanh knee from 3 dB under the ceiling, no delay. Peak limiter: 64 samples of look-ahead, all channels linked.");
        db("Ceiling", &c.ceiling_db, -12.0f, 0.0f, false, "The level the soft clip or the limiter holds, dBFS.");
        choice("LFE low-pass", &c.lfe_lowpass_hz, k_mx_hz, k_mx_hz_v, 6, "A 24 dB/octave low-pass on the LFE feed. The console's Dolby Digital encoder band-limited the LFE at 120 Hz; here the game's LFE feed is full range unless this is on.");
        choice("Mains high-pass", &c.mains_highpass_hz, k_mx_hz, k_mx_hz_v, 6, "A 24 dB/octave high-pass on the five main speakers (all buses), for small speakers with the bass sent to the LFE.");
        act(MA_SPEAKERS, "Speakers: trim and delay", "A level trim and a delay for each of the six speakers.");
        MxItem &dk = act(MA_DUCK, "Ducking", "One slot: a bus goes down while another sounds.");
        if (c.duck.enabled)
            snprintf(dk.text, sizeof dk.text, "%s under %s, %.1f dB", apu_bus_name(c.duck.target),
                     apu_bus_name(c.duck.source), (double)c.duck.depth_db);
        else
            snprintf(dk.text, sizeof dk.text, "Off");
        act(MA_RESET_OVERS, "Reset the over-full-scale counters", NULL);
    } else if (s_mx_view == MX_BUS) {
        apu_bus_params &p = c.bus[s_mx_bus];
        db("Gain", &p.gain_db, -60.0f, 12.0f, true, "The whole bus.");
        { MxItem &m = add(MK_BOOL, "Mute", NULL); m.i = &p.mute; }
        { MxItem &m = add(MK_BOOL, "Solo", "Only the soloed buses sound."); m.i = &p.solo; }
        choice("Route", &p.route, k_mx_route, k_mx_route_v, 3, "The game's panning: its own speaker sends, with trims and field shaping (for sounds the game positions). Sends: six sends of your own at the sound's level. A place in the room: the sound panned to a position in the 5.1 plane, constant power (for music, the player's engine and exhaust, speech, UI).");
        if (p.route == APU_BUS_ROUTE_PAN) {
            MxItem &mv = act(MA_PAN_MOVE, "Position", "A / Enter, then the D-pad or stick moves the sound in the room; A or B when done. Or drag in the diagram with the mouse. The room's corners are the speakers (FL, FR, SL, SR), C is the middle of the front wall, the listener sits in the middle.");
            snprintf(mv.text, sizeof mv.text, "%s", s_mx_grab ? "moving..." : "move");
            unit("Position X (left - right)", &p.pan_x, -1.0f, 1.0f, "-1 is the left wall, +1 the right one.");
            unit("Position Y (rear - front)", &p.pan_y, -1.0f, 1.0f, "-1 is the rear wall, +1 the front one, 0 the listener's row: there all five speakers share the sound.");
            unit("Width", &p.pan_width, 0.0f, 1.0f, "A stereo sound's left and right channels sit this far to the left and right of the position (1 at the front: left on FL, right on FR). A mono sound is put at both places with half its power each, which spreads it to the neighbouring speakers.");
            unit("Centre divergence", &p.pan_divergence, 0.0f, 1.0f, "0: what falls on the centre speaker stays there. 1: it is played by FL and FR instead, at -3 dB each (a phantom centre).");
        } else if (p.route == APU_BUS_ROUTE_TITLE) {
            unit("Field width", &p.field_width, 0.0f, 2.0f, "The game's left/right spread of each sound, front pair and surround pair alike: 1 as the game pans, 0 everything on the centre line, 2 wider. A sound's power stays the same.");
            unit("Centre divergence", &p.field_divergence, -1.0f, 1.0f, "Above 0: that share of what the game sends to the centre speaker goes to FL and FR instead. Below 0: that share of what FL and FR have in common goes to the centre. 0 as the game pans.");
            db("Front trim", &p.trim_db[APU_BUS_TRIM_FRONT], -60.0f, 12.0f, true, "Front left and right, relative to the game's own panning.");
            db("Centre trim", &p.trim_db[APU_BUS_TRIM_CENTRE], -60.0f, 12.0f, true, "The centre, relative to the game's own panning.");
            db("Rear trim", &p.trim_db[APU_BUS_TRIM_REAR], -60.0f, 12.0f, true, "The surrounds, relative to the game's own panning.");
            db("LFE trim", &p.trim_db[APU_BUS_TRIM_LFE], -60.0f, 12.0f, true, "What the game itself sends to the LFE from this bus.");
        } else {
            static const char *const sn[6] = { "Send to front left", "Send to front right", "Send to centre",
                                               "Send to LFE (full range)", "Send to surround left", "Send to surround right" };
            static const int order[6] = { 0, 1, 2, 4, 5, 3 };
            for (int k = 0; k < 6; k++)
                db(sn[order[k]], &p.send_db[order[k]], -60.0f, 12.0f, true,
                   "0 dB is the sound's own level. A: off / 0 dB. Left / right: 0.5 dB a step, faster when held; under -60 dB the send is off.");
        }
        db("LFE send (through the crossover)", &p.lfe_send_db, -40.0f, 12.0f, true, "An extra send of the bus to the LFE through the low-pass below, on top of the route. Off by default; A switches it between off and -10 dB.");
        it[n - 1].on_db = -10.0f;
        choice("LFE send crossover", &p.lfe_crossover_hz, k_mx_xo, k_mx_hz_v, 6, "The 24 dB/octave low-pass on that send.");
        act(MA_BUS_RESET, "Reset this bus", NULL);
    } else if (s_mx_view == MX_SPEAKERS) {
        static const char *const tn[6] = { "Front left trim", "Front right trim", "Centre trim", "LFE trim",
                                           "Surround left trim", "Surround right trim" };
        static const char *const dn[6] = { "Front left delay", "Front right delay", "Centre delay", "LFE delay",
                                           "Surround left delay", "Surround right delay" };
        for (int s = 0; s < 6; s++)
            db(tn[s], &c.speaker_trim_db[s], -24.0f, 12.0f, false, "After the mix, before the limiter. On a stereo output only the front pair applies.");
        for (int s = 0; s < 6; s++) {
            MxItem &m = add(MK_MS, dn[s], "0 to 50 ms, in steps of 0.5 ms (24 samples): 1 ms is 34 cm of path.");
            m.f = &c.speaker_delay_ms[s]; m.mn = 0.0f; m.mx = 50.0f; m.step = 0.5f;
        }
    } else {
        for (int b = 0; b < nbus; b++) {
            snprintf(s_mx_busnames[b], sizeof s_mx_busnames[b], "%s", apu_bus_name(b));
            s_mx_busname_p[b] = s_mx_busnames[b];
            s_mx_busvals[b] = b;
        }
        { MxItem &m = add(MK_BOOL, "Ducking", "The target bus goes down by the depth while the source bus is above the threshold."); m.i = &c.duck.enabled; }
        choice("Source", &c.duck.source, s_mx_busname_p, s_mx_busvals, nbus, "The bus that is listened to.");
        choice("Target", &c.duck.target, s_mx_busname_p, s_mx_busvals, nbus, "The bus that goes down.");
        db("Threshold", &c.duck.threshold_db, -60.0f, 0.0f, false, "The source's level, dBFS RMS over a 0.67 ms frame, above which the target is ducked.");
        db("Depth", &c.duck.depth_db, 0.5f, 24.0f, false, "How far the target goes down, dB.");
        { MxItem &m = add(MK_MS, "Attack", "Time constant of the way down."); m.f = &c.duck.attack_ms; m.mn = 1.0f; m.mx = 500.0f; m.step = 5.0f; }
        { MxItem &m = add(MK_MS, "Release", "Time constant of the way back."); m.f = &c.duck.release_ms; m.mn = 10.0f; m.mx = 3000.0f; m.step = 50.0f; }
    }
    return n;
}

static bool mx_selectable(const MxItem &m) { return m.kind != MK_INFO; }

/* For tests (overlay_shot): the selection onto the row with this label in
 * the view that shows; which view shows (-1: the page is closed). */
extern "C" int xbox_OverlayMixerSelect(const char *label)
{
    static MxItem items[40];
    apu_bus_config c;
    if (!s_mx_open) return 0;
    if (s_mx_live_b) c = s_mx_b;
    else if (s_mx_save_pending) c = s_mx_save_cfg;
    else apu_bus_get_config(&c);
    int n = mx_items(c, items, 40);
    for (int i = 0; i < n; i++)
        if (mx_selectable(items[i]) && !strcmp(items[i].label, label)) {
            s_mx_sel[s_mx_view] = i;
            s_mx_scroll = true;
            return 1;
        }
    return 0;
}

extern "C" int xbox_OverlayMixerView(void) { return s_mx_open ? s_mx_view : -1; }

/* Left / right on an item; true when the configuration changed. */
static bool mx_adjust(MxItem &m, int dir, apu_bus_config &c)
{
    switch (m.kind) {
    case MK_BUS:
    case MK_DB: {
        float v = *m.f;
        if (v <= APU_BUS_OFF_DB) {
            /* From off: in at -20 dB (audible), not at the bottom. */
            if (dir < 0) return false;
            v = m.mn < -20.0f ? -20.0f : m.mn;
        } else {
            v = floorf(v / m.step + 0.5f) * m.step + (float)dir * m.step;
            if (v < m.mn) v = m.off_below ? APU_BUS_OFF_DB : m.mn;
            if (v > m.mx) v = m.mx;
        }
        if (v == *m.f) return false;
        *m.f = v;
        return true;
    }
    case MK_UNIT:
    case MK_MS: {
        float v = floorf(*m.f / m.step + 0.5f) * m.step + (float)dir * m.step;
        if (v < m.mn) v = m.mn;
        if (v > m.mx) v = m.mx;
        if (v == *m.f) return false;
        *m.f = v;
        return true;
    }
    case MK_BOOL:
        *m.i = !*m.i;
        return true;
    case MK_CHOICE: {
        int k = 0;
        for (int j = 0; j < m.nvals; j++)
            if (m.vals[j] == *m.i) k = j;
        k = (k + (dir > 0 ? 1 : -1) + m.nvals) % m.nvals;
        *m.i = m.vals[k];
        return true;
    }
    case MK_ACTION:
        dir = dir > 0 ? 1 : -1;
        if (m.action == MA_BREF) {
            s_mx_b_idx = (s_mx_b_idx + dir + mx_preset_count()) % mx_preset_count();
            mx_b_load();
            if (s_mx_live_b) {
                apu_bus_set_config(&s_mx_b);
                c = s_mx_b;
            }
        } else if (m.action == MA_PRESET) {
            s_mx_preset_idx = (s_mx_preset_idx + dir + mx_preset_count()) % mx_preset_count();
        } else if (m.action == MA_AUD) {
            char path[1200];
            mx_aud_path(path, sizeof path);
            if (dir > 0) {
                if (apu_bus_audition_loop_file(path) != 0)
                    snprintf(s_mx_note, sizeof s_mx_note, "No saved clip (mixer-audition.clip) to loop.");
            } else {
                apu_bus_audition_stop();
            }
        }
        return false;
    default:
        return false;
    }
}

/* A / Enter on an item; true when the configuration changed. */
static bool mx_activate(MxItem &m, apu_bus_config &c)
{
    if (m.kind == MK_BOOL)
        return mx_adjust(m, 1, c);
    if (m.kind == MK_DB && m.off_below) {
        /* A on a level: off, or back in at its usual level. */
        *m.f = *m.f <= APU_BUS_OFF_DB ? m.on_db : APU_BUS_OFF_DB;
        return true;
    }
    if (m.kind == MK_BUS) {
        s_mx_view = MX_BUS;
        s_mx_bus = m.bus;
        s_mx_sel[MX_BUS] = 0;
        s_mx_scroll = true;
        return false;
    }
    if (m.kind != MK_ACTION)
        return false;
    switch (m.action) {
    case MA_ENABLE: {
        int on = !apu_bus_enabled();
        xbox_SettingSet(XS_AUDIO_MIXER, on);
        changed();
        if (!on) s_mx_live_b = false;
        mx_scan_user();
        return false;
    }
    case MA_AB:
        mx_ab_toggle();
        apu_bus_get_config(&c);
        return false;
    case MA_PRESET: {
        apu_bus_config p;
        if (mx_preset_get(s_mx_preset_idx, &p)) {
            if (s_mx_live_b) mx_ab_toggle();
            c = p;
            snprintf(s_mx_note, sizeof s_mx_note, "Loaded \"%s\" into A.", mx_preset_name(s_mx_preset_idx));
            return true;
        }
        snprintf(s_mx_note, sizeof s_mx_note, "\"%s\" could not be read.", mx_preset_name(s_mx_preset_idx));
        return false;
    }
    case MA_SAVE_COPY: {
        char dir[1024], path[1200];
        apu_bus_config a = s_mx_live_b ? s_mx_save_cfg : c;
        mx_dir(dir, sizeof dir);
        for (int k = 1; k < 100; k++) {
            snprintf(path, sizeof path, "%s/mixer-%d.toml", dir, k);
            FILE *f = fopen(path, "rb");
            if (f) { fclose(f); continue; }
            if (apu_bus_save_file(path, &a) == 0)
                snprintf(s_mx_note, sizeof s_mx_note, "Saved mixer-%d.toml beside mixer.toml.", k);
            else
                snprintf(s_mx_note, sizeof s_mx_note, "Could not write %s.", path);
            break;
        }
        mx_scan_user();
        return false;
    }
    case MA_PAN_MOVE: s_mx_grab = !s_mx_grab; return false;
    case MA_AUD:
        if (apu_bus_audition_state() != APU_BUS_AUD_OFF)
            apu_bus_audition_stop();
        else if (apu_bus_audition_loop_last() != 0)
            snprintf(s_mx_note, sizeof s_mx_note, apu_bus_audition_kept_seconds()
                     ? "Not enough recorded yet: play for at least five seconds, then loop."
                     : "No recording yet: set Audition recording to 30 or 60 s, play a while, then loop.");
        return false;
    case MA_AUD_SAVE: {
        char path[1200];
        mx_aud_path(path, sizeof path);
        if (apu_bus_audition_save(path) == 0)
            snprintf(s_mx_note, sizeof s_mx_note, "Saved the clip as mixer-audition.clip beside mixer.toml.");
        else
            snprintf(s_mx_note, sizeof s_mx_note, "Could not write %s.", path);
        return false;
    }
    case MA_SPEAKERS: s_mx_view = MX_SPEAKERS; s_mx_scroll = true; return false;
    case MA_DUCK:     s_mx_view = MX_DUCK; s_mx_scroll = true; return false;
    case MA_RESET_OVERS: apu_bus_reset_overs(); return false;
    case MA_BUS_RESET: {
        apu_bus_config nz;
        apu_bus_config_neutral(&nz);
        c.bus[s_mx_bus] = nz.bus[s_mx_bus];
        return true;
    }
    default: return false;
    }
}

static void mx_value(const MxItem &m, char *out, size_t n)
{
    switch (m.kind) {
    case MK_BUS:
    case MK_DB:
        if (*m.f <= APU_BUS_OFF_DB) snprintf(out, n, "off");
        else snprintf(out, n, "%+.1f dB", (double)*m.f);
        break;
    case MK_MS: snprintf(out, n, "%.1f ms", (double)*m.f); break;
    case MK_UNIT: snprintf(out, n, "%+.2f", (double)*m.f); break;
    case MK_BOOL: snprintf(out, n, "%s", *m.i ? "On" : "Off"); break;
    case MK_CHOICE: {
        const char *t = "";
        for (int j = 0; j < m.nvals; j++)
            if (m.vals[j] == *m.i) t = m.names[j];
        if (!t[0]) { snprintf(out, n, "%d", *m.i); break; }
        snprintf(out, n, "%s", t);
        break;
    }
    default: snprintf(out, n, "%s", m.text); break;
    }
}

/* A level meter: -60..0 dBFS over w, green to -12, amber to -3, red above. */
static void mx_meter(ImDrawList *dl, ImVec2 p, float w, float h, float db, float peak_db, bool over)
{
    auto x_of = [&](float d) { float t = (d + 60.0f) / 60.0f; return p.x + w * (t < 0 ? 0 : t > 1 ? 1 : t); };
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(T->track_off), h * 0.3f);
    if (db > -60.0f) {
        float x1 = x_of(db), a = x_of(-12.0f), b = x_of(-3.0f);
        dl->AddRectFilled(p, ImVec2(x1 < a ? x1 : a, p.y + h), col(T->perf_good), h * 0.3f);
        if (x1 > a) dl->AddRectFilled(ImVec2(a, p.y), ImVec2(x1 < b ? x1 : b, p.y + h), col(T->perf_warn));
        if (x1 > b) dl->AddRectFilled(ImVec2(b, p.y), ImVec2(x1, p.y + h), col(T->perf_bad));
    }
    if (peak_db > -60.0f) {
        float x = x_of(peak_db);
        dl->AddRectFilled(ImVec2(x - 1.0f, p.y), ImVec2(x + 1.0f, p.y + h), col(T->text_hi));
    }
    if (over)
        dl->AddRectFilled(ImVec2(p.x + w + h * 0.3f, p.y), ImVec2(p.x + w + h * 1.3f, p.y + h), col(T->perf_bad), h * 0.3f);
}

/* The room from above, for a bus in pan mode: the five speakers on its
 * edge (their dots grow with what each gets, in dB under it), the listener
 * in the middle, the sound's position; with a width, its two ends. Dragging
 * in it moves the position. True when the mouse changed it. */
static bool mx_pan_diagram(ImDrawList *dl, ImVec2 at, float w, float h, float fs, apu_bus_params &p)
{
    static const struct { float x, y; const char *name; } spk[5] = {
        { -1, 1, "FL" }, { 1, 1, "FR" }, { 0, 1, "C" }, { -1, -1, "SL" }, { 1, -1, "SR" },
    };
    const float pad = fs * 2.4f;
    float side = (w < h ? w : h) - 2.0f * pad;
    if (side < fs * 3.0f)
        return false;
    const ImVec2 o(at.x + (w - side) * 0.5f, at.y + (h - side) * 0.5f);
    auto pt = [&](float x, float y) { return ImVec2(o.x + (x + 1.0f) * 0.5f * side, o.y + (1.0f - y) * 0.5f * side); };
    bool changed_ = false;

    ImGui::SetCursorScreenPos(ImVec2(o.x - fs * 0.5f, o.y - fs * 0.5f));
    ImGui::InvisibleButton("##room", ImVec2(side + fs, side + fs));
    if (ImGui::IsItemActive()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        float x = (m.x - o.x) / side * 2.0f - 1.0f, y = 1.0f - (m.y - o.y) / side * 2.0f;
        x = floorf((x < -1 ? -1 : x > 1 ? 1 : x) * 20.0f + 0.5f) / 20.0f;
        y = floorf((y < -1 ? -1 : y > 1 ? 1 : y) * 20.0f + 0.5f) / 20.0f;
        if (x != p.pan_x || y != p.pan_y) {
            p.pan_x = x;
            p.pan_y = y;
            changed_ = true;
        }
        s_last_input_pad = false;
    }

    dl->AddRect(o, ImVec2(o.x + side, o.y + side), col(T->rule), fs * 0.3f);
    dl->AddLine(pt(-1, 0), pt(1, 0), col(T->rule, 0.5f));
    dl->AddLine(pt(0, -1), pt(0, 1), col(T->rule, 0.5f));
    /* The listener, facing the front wall. */
    {
        const ImVec2 c = pt(0, 0);
        dl->AddCircle(c, fs * 0.45f, col(T->hint), 16, 1.5f);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - fs * 0.85f), ImVec2(c.x - fs * 0.3f, c.y - fs * 0.45f),
                              ImVec2(c.x + fs * 0.3f, c.y - fs * 0.45f), col(T->hint));
    }
    /* What each speaker gets: a mono sound's gains (the two ends' powers
     * averaged); for a stereo sound the left end's and the right end's
     * are the same picture, mirrored about the position. */
    {
        float gl[5], gr[5];
        const float xl = p.pan_x - p.pan_width < -1 ? -1 : p.pan_x - p.pan_width;
        const float xr = p.pan_x + p.pan_width > 1 ? 1 : p.pan_x + p.pan_width;
        apu_bus_pan_gains(xl, p.pan_y, p.pan_divergence, gl);
        apu_bus_pan_gains(xr, p.pan_y, p.pan_divergence, gr);
        for (int k = 0; k < 5; k++) {
            const float pw = 0.5f * (gl[k] * gl[k] + gr[k] * gr[k]);
            const float db = pw > 1e-6f ? 10.0f * log10f(pw) : -99.0f;
            const ImVec2 c = pt(spk[k].x, spk[k].y);
            const float lvl = db < -30.0f ? 0.0f : (db + 30.0f) / 30.0f;
            char t[24];
            dl->AddCircleFilled(c, fs * (0.35f + 0.55f * lvl), lvl > 0 ? col(T->fill, 0.35f + 0.65f * lvl) : col(T->track_off));
            dl->AddCircle(c, fs * 0.9f, col(T->hint), 20, 1.0f);
            if (db > -60.0f) snprintf(t, sizeof t, "%s %.1f", spk[k].name, (double)db);
            else snprintf(t, sizeof t, "%s  -", spk[k].name);
            const float tw_ = tw(fs, t);
            txt(dl, fs, ImVec2(c.x - tw_ * 0.5f, spk[k].y > 0 ? c.y - fs * 2.2f : c.y + fs * 1.1f),
                lvl > 0 ? col(T->text_hi) : col(T->hint), t);
        }
        /* The sound: its two ends when it has a width, and the position. */
        if (p.pan_width > 0.0f) {
            dl->AddLine(pt(xl, p.pan_y), pt(xr, p.pan_y), col(T->sel_bar), 2.0f);
            dl->AddCircleFilled(pt(xl, p.pan_y), fs * 0.3f, col(T->sel_bar));
            dl->AddCircleFilled(pt(xr, p.pan_y), fs * 0.3f, col(T->sel_bar));
            txt(dl, fs, ImVec2(pt(xl, p.pan_y).x - fs * 0.25f, pt(xl, p.pan_y).y - fs * 1.5f), col(T->hint), "L");
            txt(dl, fs, ImVec2(pt(xr, p.pan_y).x - fs * 0.25f, pt(xr, p.pan_y).y - fs * 1.5f), col(T->hint), "R");
        }
        dl->AddCircleFilled(pt(p.pan_x, p.pan_y), fs * 0.45f, s_mx_grab ? col(T->warn) : col(T->text_hi));
        dl->AddCircle(pt(p.pan_x, p.pan_y), fs * 0.7f, s_mx_grab ? col(T->warn) : col(T->sel_bar), 20, 2.0f);
    }
    return changed_;
}

static void build_mixer(float fs, ImDrawList *dl)
{
    ImGuiIO &io = ImGui::GetIO();
    apu_bus_config c;
    static MxItem items[40];
    apu_bus_meters mt;
    bool dirty = false;

    mx_poll();
    if (s_mx_live_b) c = s_mx_b;
    else if (s_mx_save_pending) c = s_mx_save_cfg;
    else apu_bus_get_config(&c);
    apu_bus_get_meters(&mt);

    int n = mx_items(c, items, 40);
    int &sel = s_mx_sel[s_mx_view];
    if (sel < 0 || sel >= n || !mx_selectable(items[sel])) {
        sel = 0;
        while (sel < n - 1 && !mx_selectable(items[sel])) sel++;
    }

    collect_steps();
    /* A note (preset loaded, copy saved) stays until the next input. */
    if (s_steps[D_UP] | s_steps[D_DOWN] | s_steps[D_LEFT] | s_steps[D_RIGHT] | s_activate)
        s_mx_note[0] = 0;
    auto move = [&](int dir) {
        for (int k = 0; k < n; k++) {
            sel = (sel + dir + n) % n;
            if (mx_selectable(items[sel])) break;
        }
        s_mx_scroll = true;
    };
    /* A held direction speeds up: 2x after six steps, 4x after fourteen,
     * 8x after thirty. Single presses are always one step. */
    static int streak[D_COUNT];
    int mult[D_COUNT];
    for (int d = 0; d < D_COUNT; d++) {
        const bool held = s_dpad[d] || s_stick[d] || s_key[d];
        streak[d] = held ? streak[d] + s_steps[d] : 0;
        mult[d] = streak[d] > 30 ? 8 : streak[d] > 14 ? 4 : streak[d] > 6 ? 2 : 1;
    }
    const bool pan_view = s_mx_view == MX_BUS && c.bus[s_mx_bus].route == APU_BUS_ROUTE_PAN;
    if (!pan_view)
        s_mx_grab = false;
    if (s_mx_grab) {
        /* The position follows the D-pad / stick, 0.05 a step. */
        apu_bus_params &p = c.bus[s_mx_bus];
        const float dx = 0.05f * (float)(s_steps[D_RIGHT] * mult[D_RIGHT] - s_steps[D_LEFT] * mult[D_LEFT]);
        const float dy = 0.05f * (float)(s_steps[D_UP] * mult[D_UP] - s_steps[D_DOWN] * mult[D_DOWN]);
        if (dx != 0.0f || dy != 0.0f) {
            float x = floorf((p.pan_x + dx) * 20.0f + 0.5f) / 20.0f, y = floorf((p.pan_y + dy) * 20.0f + 0.5f) / 20.0f;
            p.pan_x = x < -1.0f ? -1.0f : x > 1.0f ? 1.0f : x;
            p.pan_y = y < -1.0f ? -1.0f : y > 1.0f ? 1.0f : y;
            dirty = true;
        }
        memset(s_steps, 0, sizeof s_steps);
        if (s_activate > 0) { s_mx_grab = false; s_activate = 0; }
    }
    while (s_steps[D_UP]-- > 0) move(-1);
    while (s_steps[D_DOWN]-- > 0) move(1);
    while (s_steps[D_LEFT]-- > 0) dirty |= mx_adjust(items[sel], -mult[D_LEFT], c);
    while (s_steps[D_RIGHT]-- > 0) dirty |= mx_adjust(items[sel], mult[D_RIGHT], c);
    memset(s_steps, 0, sizeof s_steps);
    int view0 = s_mx_view;
    while (s_activate-- > 0 && s_mx_view == view0) dirty |= mx_activate(items[sel], c);
    s_activate = 0;
    if (s_mx_view == view0 && items[sel].kind == MK_BUS) {
        apu_bus_params &p = c.bus[items[sel].bus];
        while (s_page_delta < 0) { p.mute = !p.mute; s_page_delta++; dirty = true; }
        while (s_page_delta > 0) { p.solo = !p.solo; s_page_delta--; dirty = true; }
        if (s_reset > 0) {
            apu_bus_config nz;
            apu_bus_config_neutral(&nz);
            p = nz.bus[items[sel].bus];
            dirty = true;
        }
    } else if (s_mx_view == view0 && s_reset > 0 && (items[sel].kind == MK_DB || items[sel].kind == MK_MS || items[sel].kind == MK_UNIT)) {
        /* Y / Del: the neutral value of the row. */
        apu_bus_config nz;
        apu_bus_config_neutral(&nz);
        size_t off = (size_t)((const char *)items[sel].f - (const char *)&c);
        float v = *(const float *)((const char *)&nz + off);
        if (v != *items[sel].f) { *items[sel].f = v; dirty = true; }
    }
    s_page_delta = 0;
    s_reset = 0;
    if (s_mx_view == MX_MAIN && view0 == MX_MAIN && s_mx_aud_keep != apu_bus_audition_kept_seconds()) {
        apu_bus_audition_keep(s_mx_aud_keep);   /* the Audition recording row */
        dirty = false;
    }
    if (dirty)
        mx_changed(c);
    if (s_mx_view != view0) {
        ImGui::Dummy(ImVec2(0, 0));
        return;                         /* the new view from the next frame */
    }

    /* The title line. */
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        char t[160];
        if (s_mx_view == MX_BUS) snprintf(t, sizeof t, "Mixer  >  %s", apu_bus_name(s_mx_bus));
        else if (s_mx_view == MX_SPEAKERS) snprintf(t, sizeof t, "Mixer  >  Speakers");
        else if (s_mx_view == MX_DUCK) snprintf(t, sizeof t, "Mixer  >  Ducking");
        else snprintf(t, sizeof t, "Mixer");
        dl->AddText(ImVec2(p.x, p.y), col(T->text_hi), t);
        if (apu_bus_enabled()) {
            const char *l = apu_bus_audition_state() != APU_BUS_AUD_OFF
                ? (s_mx_live_b ? "AUDITION LOOP, B" : "AUDITION LOOP, A")
                : s_mx_live_b ? "listening to B" : "listening to A";
            float w = ImGui::CalcTextSize(l).x;
            dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - w - fs * 0.5f, p.y),
                        s_mx_live_b ? col(T->warn) : col(T->hint), l);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + fs * 1.6f));
    }

    const char *hint = items[sel].hint ? items[sel].hint : "";
    const float wrap_w = ImGui::GetContentRegionAvail().x;
    const float hs = LY.hint;           /* the footer's text size */
    const float hint_h = ImGui::GetFont()->CalcTextSizeA(hs, FLT_MAX, wrap_w, hint[0] ? hint : "A").y;
    const float meters_h = apu_bus_enabled() ? hs * 3.4f : 0.0f;
    const float footer = hs * 0.5f + meters_h + hint_h + hs * 0.8f;
    const float rh = LY.pitch * 0.82f;       /* a denser list: the Mixer has more rows */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 list_pos = ImGui::GetCursorScreenPos();
    const float list_h = avail.y - footer;
    const float list_w = pan_view ? avail.x * 0.56f : avail.x;
    ImGui::BeginChild("mixer", ImVec2(list_w, list_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
    gesture_scroll();
    ImDrawList *cdl = ImGui::GetWindowDrawList();
    const float rw = ImGui::GetContentRegionAvail().x - fs * 0.5f;
    int clicked = -1, cdir = 0;
    for (int i = 0; i < n; i++) {
        MxItem &m = items[i];
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool isel = i == sel;
        ImGui::PushID(i);
        const bool tap = ImGui::InvisibleButton("##mx", ImVec2(rw, rh)) && gesture_tap();
        if (ImGui::IsItemHovered() && mx_selectable(m) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            sel = i;
            s_last_input_pad = false;
        }
        if (tap && mx_selectable(m)) {
            clicked = i;
            /* The right fifth steps up, the fifth before it down; the rest
             * of the row is A. */
            cdir = io.MousePos.x > p.x + rw * 0.8f ? 1 : io.MousePos.x > p.x + rw * 0.6f ? -1 : 0;
        }
        ImGui::PopID();
        if (!ImGui::IsRectVisible(p, ImVec2(p.x + rw, p.y + rh)) && !(isel && s_mx_scroll))
            continue;
        if (isel) {
            cdl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), col(T->sel_fill, T->sel_fill_a), fs * 0.2f);
            arrow_tri(cdl, p.x + fs * 0.35f, p.y + rh * 0.5f, fs * 0.36f, 1, col(T->arrow));
        }
        const float ty = p.y + (rh - fs) * 0.5f;
        char v[96];
        mx_value(m, v, sizeof v);
        float vw = ImGui::CalcTextSize(v).x;
        ImU32 lc = m.kind == MK_INFO ? col(T->text_info)
                 : m.kind == MK_ACTION && !m.text[0] ? col(T->action)
                 : col(isel ? T->text_hi : T->text);
        cdl->AddText(ImVec2(p.x + fs * 0.8f, ty), lc, m.label);
        if (m.kind == MK_BUS) {
            const apu_bus_params &bp = c.bus[m.bus];
            float x = p.x + rw * 0.36f;
            if (bp.mute) { cdl->AddText(ImVec2(x, ty), col(T->warn), "M"); }
            if (bp.solo) { cdl->AddText(ImVec2(x + fs * 1.1f, ty), col(T->perf_good), "S"); }
            if (bp.route == APU_BUS_ROUTE_SENDS) cdl->AddText(ImVec2(x + fs * 2.2f, ty), col(T->hint), "sends");
            else if (bp.lfe_send_db > APU_BUS_OFF_DB || bp.trim_db[0] != 0 || bp.trim_db[1] != 0
                     || bp.trim_db[2] != 0 || bp.trim_db[3] != 0)
                cdl->AddText(ImVec2(x + fs * 2.2f, ty), col(T->hint), "trims");
            mx_meter(cdl, ImVec2(p.x + rw * 0.50f, p.y + rh * 0.36f), rw * 0.27f, rh * 0.28f,
                     mt.bus_rms_db[m.bus], -200.0f, false);
        }
        if (v[0]) {
            /* The value takes what the label leaves; smaller, then cut. */
            float room = rw - fs * 1.6f - tw(fs, m.label) - fs * 0.6f;
            if (m.kind == MK_BUS)
                room = vw;
            txt_fit(cdl, fs, ImVec2(p.x + rw - fs * 0.8f, ty), m.kind == MK_INFO ? col(T->text_info) : col(T->text_hi),
                    v, room > fs * 2.0f ? room : fs * 2.0f, true);
        }
        if (isel && s_mx_scroll) {
            ImGui::SetScrollHereY(0.5f);
            s_mx_scroll = false;
        }
    }
    ImGui::EndChild();
    if (pan_view) {
        /* Beside the list; the footer goes on from where the list ended. */
        apu_bus_params &p = c.bus[s_mx_bus];
        const ImVec2 after = ImGui::GetCursorScreenPos();
        /* The diagram's parts at the hint's size: it has the room that is left. */
        if (mx_pan_diagram(dl, ImVec2(list_pos.x + list_w + fs, list_pos.y), avail.x - list_w - fs, list_h,
                           LY.hint, p))
            mx_changed(c);
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(ImVec2(0, 0));
    }
    if (clicked >= 0) {
        sel = clicked;
        bool ch = cdir && items[clicked].kind != MK_BOOL && !(items[clicked].kind == MK_ACTION
                      && items[clicked].action != MA_BREF && items[clicked].action != MA_PRESET)
                ? mx_adjust(items[clicked], cdir, c)
                : mx_activate(items[clicked], c);
        if (ch) mx_changed(c);
    }

    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(p.x - 0.05f * LY.cw, p.y + hs * 0.2f), ImVec2(p.x + avail.x + 0.03f * LY.cw, p.y + hs * 0.2f),
                    col(T->rule));
        float y = p.y + hs * 0.5f;
        if (meters_h > 0) {
            /* The speaker feeds after the output stage: RMS bar, peak hold
             * tick, and a red block when samples went past full scale. */
            static const char *const sp6[6] = { "FL", "FR", "C", "LFE", "SL", "SR" };
            static const char *const sp2[2] = { "L", "R" };
            const int nch = mt.channels == 6 ? 6 : 2;
            const float colw = avail.x / 3.0f;
            unsigned overs = 0;
            for (int s = 0; s < nch; s++) {
                float x = p.x + colw * (float)(s % 3), yy = y + hs * 1.1f * (float)(s / 3);
                char lab[40];
                txt(dl, hs, ImVec2(x, yy), col(T->hint), nch == 6 ? sp6[s] : sp2[s]);
                mx_meter(dl, ImVec2(x + hs * 2.4f, yy + hs * 0.25f), colw - hs * 9.0f, hs * 0.5f,
                         mt.spk_rms_db[s], mt.spk_peak_db[s], mt.spk_over[s] != 0);
                if (mt.spk_peak_db[s] > -99.0f) snprintf(lab, sizeof lab, "%+.1f", (double)mt.spk_peak_db[s]);
                else snprintf(lab, sizeof lab, "-inf");
                txt(dl, hs, ImVec2(x + colw - hs * 5.2f, yy), mt.spk_peak_db[s] > 0.0f ? col(T->perf_bad) : col(T->hint), lab);
                overs += mt.spk_over[s];
            }
            char st[200];
            snprintf(st, sizeof st, "RMS, peak hold (dBFS)     over full scale: %u samples     limiter %.1f dB     duck %.1f dB",
                     overs, (double)(mt.limiter_db > 0.05f ? -mt.limiter_db : 0.0f),
                     (double)(mt.duck_db > 0.05f ? -mt.duck_db : 0.0f));
            txt_fit(dl, hs, ImVec2(p.x, y + hs * 2.3f), overs ? col(T->warn) : col(T->hint), st, avail.x);
            y += meters_h;
        }
        const char *fst = apu_bus_file_status();
        const char *line = s_mx_note[0] ? s_mx_note : fst[0] ? fst : hint;
        if (line[0])
            dl->AddText(ImGui::GetFont(), hs, ImVec2(p.x, y), (s_mx_note[0] || fst[0]) && line != hint ? col(T->warn) : col(T->hint),
                        line, nullptr, wrap_w);
        if (s_mx_grab) {
            static char grab[160];
            snprintf(grab, sizeof grab, pad_hints() ? "D-pad / stick  move the sound in the room      %s or %s  done"
                                                    : "Arrows  move the sound in the room      %s or %s  done",
                     act_name(0), act_name(1));
            foot_set(grab, true);
            return;
        }
        static char keys[240];
        if (pad_hints())
            snprintf(keys, sizeof keys, s_mx_view == MX_MAIN
                         ? "D-pad  select / change   %s  open   LB  mute   RB  solo   %s  reset   %s  back   Back+Y  A/B"
                         : "D-pad  select / change    %s  toggle    %s  default    %s  back    Back+Y  A/B",
                     act_name(0), act_name(3), act_name(1));
        else
            snprintf(keys, sizeof keys, "%s", s_mx_view == MX_MAIN
                         ? "Arrows  select / change   Enter  open   Q  mute   E  solo   Del  reset   Esc  back   F7  A/B"
                         : "Arrows  select / change    Enter  toggle    Del  default    Esc  back    F7  A/B");
        foot_set(keys);
    }
}

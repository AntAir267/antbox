/* Audition (apu_bus.h): the recorded stretch looped through the mixer.
 * Included by test_main.c. */
static float aud_noise(unsigned frame, unsigned i, unsigned ch)
{
    uint32_t x = frame * 2654435761u ^ (i * 40503u + ch * 9176u + 12345u);
    x ^= x >> 15; x *= 2246822519u; x ^= x >> 13; x *= 3266489917u; x ^= x >> 16;
    return ((float)(int32_t)x / 2147483648.0f) * 0.5f;
}

/* Frame f of the "game": music (stereo, speaker bins), an impact
 * (positioned), the engine (mixbin 17), into bins through the mixer; or,
 * while a clip loops, the clip's frame in their place, as apu_vp.c does. */
static void aud_frame(unsigned f, float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    struct apu_bus_rt *rt = apu_bus_frame_begin(6, k_map);
    tvoice v;
    memset(bins, 0, sizeof(float) * NUM_MIXBINS * NUM_SAMPLES_PER_FRAME);
    if (apu_bus_audition_playing()) {
        apu_bus_audition_mix(rt, bins);
        apu_bus_frame_end(rt, bins);
        return;
    }
    for (int k = 0; k < 3; k++) {
        static const float g[3][8] = { { 0.5f, 0.5f, 0.4f, 0.4f, 0, 0.07f, 0, 0 },
                                       { 0.5f, 0.1f, 0.2f, 0.05f, 0.3f, 0.1f, 0, 0 },
                                       { 0, 0, 0, 0, 0, 0.5f, 0, 0 } };
        static const int bus[3] = { 0, 9, 4 }, kb[3] = { 0, 2, 1 };
        memset(&v, 0, sizeof(v));
        v.channels = k == 0 ? 2 : 1;
        memcpy(v.bin, k_bins[kb[k]], sizeof(v.bin));
        memcpy(v.g, g[k], sizeof(v.g));
        for (unsigned i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            v.samples[i][0] = aud_noise(f, i, (unsigned)k * 2);
            v.samples[i][1] = k == 0 ? aud_noise(f, i, 1) : v.samples[i][0];
        }
        apu_bus_mix_voice(rt, bus[k], bins, (const float (*)[2])v.samples, v.channels, v.bin, v.g);
    }
    apu_bus_frame_end(rt, bins);
}

static double bins_rms(const float bins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    double p = 0;
    for (int b = 0; b < NUM_MIXBINS; b++) p += bin_rms(bins[b]) * bin_rms(bins[b]);
    return sqrt(p);
}

static double aud_diff(const float a[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                       const float b[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    double worst = 0;
    for (int k = 0; k < NUM_MIXBINS; k++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            double d = fabs((double)a[k][i] - b[k][i]);
            if (d > worst) worst = d;
        }
    return worst;
}

static void test_audition(void)
{
    static float live[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], loop[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static float keep[8][NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    const unsigned total = 12000;                       /* 8 s; the loop is 6 s */
    apu_bus_config cfg;
    apu_bus_audition_info ai;
    char path[512];
    const char *tmp = getenv("TMPDIR");
    double mid = 0, worst = 0, wrap_min = 1e9, wrap_max = 0;
    size_t held;

    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
    apu_bus_enable(1);

    /* Recording off (the default): nothing is held, nothing can loop. */
    aud_frame(0, live);
    apu_bus_audition_get(&ai);
    CHECK(ai.bytes == 0 && ai.kept_s == 0.0f, "audition holds %zu bytes with recording off", ai.bytes);
    CHECK(apu_bus_audition_loop_last() != 0, "a loop starts with nothing recorded");

    /* Recording on: 8 s of the game. Frames 5000..5007 are kept to compare
     * with. Too little recorded does not loop. */
    apu_bus_audition_keep(30);
    for (unsigned f = 0; f < total; f++) {
        aud_frame(f, live);
        if (f == 3000) CHECK(apu_bus_audition_loop_last() != 0, "2 s recorded and it loops");
        if (f >= 5000 && f < 5008) memcpy(keep[f - 5000], live, sizeof(live));
    }
    apu_bus_audition_get(&ai);
    held = ai.bytes;
    CHECK(fabsf(ai.kept_s - 8.0f) < 0.01f, "8 s played, %.2f s kept", ai.kept_s);
    CHECK(apu_bus_audition_loop_last() == 0, "8 s recorded do not loop");

    /* The loop: frame p of it is frame p of the game, to the 16-bit
     * quantisation of the samples. */
    for (unsigned p = 0; p < 5008; p++) {
        aud_frame(0, loop);
        if (p == 0) CHECK(apu_bus_audition_state() == APU_BUS_AUD_LAST, "the loop did not start");
        if (p >= 5000) {
            double d = aud_diff(loop, keep[p - 5000]);
            if (d > worst) worst = d;
            mid += bins_rms(loop) / 8.0;
        }
    }
    CHECK(worst < 5e-5, "the loop differs from the live sum by %.2e (a 16-bit step is 3e-5)", worst);
    printf("audition: 30 s asked for holds %.0f MB; loop against live, largest sample difference %.1e (%.0f dB)\n",
           held / 1048576.0, worst, db(worst));
    apu_bus_audition_get(&ai);
    CHECK(fabsf(ai.length_s - 6.0f) < 0.01f && fabsf(ai.position_s - 5008.0f / 1500.0f) < 0.01f,
          "loop length %.2f s, position %.2f s", ai.length_s, ai.position_s);

    /* The mixer acts on it: music muted, the loop's level drops; back. */
    cfg.bus[0].mute = 1;
    apu_bus_set_config(&cfg);
    aud_frame(0, loop);
    CHECK(bins_rms(loop) < mid * 0.75, "muting Music does not change the looped sound");
    cfg.bus[0].mute = 0;
    apu_bus_set_config(&cfg);

    /* Over the loop point: no gap and no bump (noise, so equal power holds). */
    for (unsigned p = 5009; p < 9000 + 3200; p++) {
        double r;
        aud_frame(0, loop);
        if (p < 9000 - 200) continue;
        r = bins_rms(loop);
        if (r < wrap_min) wrap_min = r;
        if (r > wrap_max) wrap_max = r;
    }
    CHECK(db(wrap_min / mid) > -3.0 && db(wrap_max / mid) < 3.0,
          "level across the loop point: %.1f .. %.1f dB of the middle", db(wrap_min / mid), db(wrap_max / mid));

    /* Saved; stopped (the live sound is back, the recording starts afresh);
     * the saved clip loops the same. */
    snprintf(path, sizeof(path), "%s/apu_bus_test_%ld.clip", tmp && *tmp ? tmp : ".", (long)getpid());
    CHECK(apu_bus_audition_save(path) == 0, "the clip does not save");
    apu_bus_audition_stop();
    aud_frame(7, live);
    CHECK(apu_bus_audition_state() == APU_BUS_AUD_OFF && bins_rms(live) > 0.05, "the live sound is not back after the loop");
    apu_bus_audition_get(&ai);
    CHECK(ai.kept_s < 0.01f, "the recording did not start afresh (%.2f s)", ai.kept_s);
    CHECK(apu_bus_audition_save(path) != 0, "a clip saves with no loop playing");
    CHECK(apu_bus_audition_loop_file(path) == 0, "the saved clip does not load");
    worst = 0;
    for (unsigned p = 0; p < 5008; p++) {
        aud_frame(0, loop);
        if (p >= 5000) {
            double d = aud_diff(loop, keep[p - 5000]);
            if (d > worst) worst = d;
        }
    }
    CHECK(apu_bus_audition_state() == APU_BUS_AUD_FILE && worst < 5e-5, "the saved clip differs by %.2e", worst);
    CHECK(apu_bus_audition_loop_file("/nonexistent/clip") != 0, "a missing clip loads");
    apu_bus_audition_stop();
    aud_frame(8, live);
    CHECK(apu_bus_audition_state() == APU_BUS_AUD_OFF, "the saved clip's loop does not stop");
    remove(path);

    /* Recording off again: the store is given back. Then on once more, for
     * the identity test that follows: recording copies what is mixed, it
     * does not change it. */
    apu_bus_audition_keep(0);
    aud_frame(9, live);
    apu_bus_audition_get(&ai);
    CHECK(ai.bytes < held / 2, "recording off still holds %zu bytes", ai.bytes);
    apu_bus_audition_keep(30);
    aud_frame(10, live);
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);
}

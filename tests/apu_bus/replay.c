/* apu_bus_replay <recording> : a title's mix loop, recorded with
 * RECOMP_MIX_TRACE_REC while the mixer was off, summed again three ways:
 *
 *   A  the loop of apu_vp.c as written (common.h)
 *   B  the host mixer on with the neutral configuration (apu_bus.c)
 *
 * and compared: A's mixbins with the ones the game itself produced (the
 * recording's 'F' records: that A is the game's sum), B's mixbins with A's,
 * and the DSP stage's s16 output of A with the mixer off against B's with
 * the mixer on, stereo and 5.1. Bytes, not levels. */
#include "common.h"
#include <time.h>

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Everything on at once: what the mixer costs at most. */
static void busy_config(apu_bus_config *c)
{
    apu_bus_config_neutral(c);
    for (int b = 0; b < 13; b++) {
        c->bus[b].gain_db = -1.0f;
        /* A third each: sends, a place in the room, the title's panning
         * with the field shaping. */
        c->bus[b].route = b % 3 == 0 ? APU_BUS_ROUTE_SENDS : b % 3 == 1 ? APU_BUS_ROUTE_PAN : APU_BUS_ROUTE_TITLE;
        c->bus[b].pan_x = 0.3f; c->bus[b].pan_y = 0.4f; c->bus[b].pan_width = 0.5f; c->bus[b].pan_divergence = 0.5f;
        c->bus[b].field_width = 0.7f; c->bus[b].field_divergence = 0.3f;
        for (int t = 0; t < APU_BUS_TRIMS; t++) c->bus[b].trim_db[t] = -0.5f;
        for (int s = 0; s < APU_BUS_SPEAKERS; s++) c->bus[b].send_db[s] = -6.0f;
        c->bus[b].lfe_send_db = -6.0f;
        c->bus[b].lfe_crossover_hz = 80;
    }
    for (int s = 0; s < APU_BUS_SPEAKERS; s++) {
        c->speaker_trim_db[s] = -1.0f;
        c->speaker_delay_ms[s] = 2.0f;
    }
    c->lfe_lowpass_hz = 120;
    c->mains_highpass_hz = 80;
    c->limiter = APU_BUS_LIM_PEAK;
    c->ceiling_db = -1.0f;
    c->duck.enabled = 1;
    c->duck.source = 9;
    c->duck.target = 0;
}

int main(int argc, char **argv)
{
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    static float A[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], B[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static float R[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    struct { uint8_t tag, channels; uint16_t v; int32_t bin[8]; float g[8]; } h;
    static tvoice vv[512];
    unsigned long frames = 0, voices = 0, bad_game = 0, bad_bins = 0, bad_st = 0, bad_51 = 0;
    unsigned long sounding = 0, lim_quiet = 0, lim_quiet_bad = 0, lim_loud = 0, lim_loud_bad = 0;
    unsigned long fold_quiet = 0, fold_quiet_bad = 0, fold_loud = 0, fold_loud_bad = 0;
    int fold_peak = 0;
    double peak = 0;
    int nv = 0, c;
    apu_bus_config cfg, busy;
    double t_old = 0, t_neutral = 0, t_busy = 0, t0;
    static float feeds[6][NUM_SAMPLES_PER_FRAME];

    busy_config(&busy);
    if (!f) { fprintf(stderr, "usage: apu_bus_replay <recording>\n"); return 2; }
    dsp_setup();
    apu_bus_set_names(k_names, 13);
    apu_bus_config_neutral(&cfg);
    apu_bus_set_config(&cfg);

    while ((c = fgetc(f)) != EOF) {
        if (c == 'V') {
            ungetc(c, f);
            if (fread(&h, sizeof(h), 1, f) != 1) break;
            if (nv >= 512) { fprintf(stderr, "too many voices in a frame\n"); return 2; }
            vv[nv].channels = h.channels;
            for (int b = 0; b < 8; b++) { vv[nv].bin[b] = h.bin[b]; vv[nv].g[b] = h.g[b]; }
            if (fread(vv[nv].samples, sizeof(float) * 2, NUM_SAMPLES_PER_FRAME, f) != NUM_SAMPLES_PER_FRAME) break;
            for (int b = 0; b < 8; b++) if (h.g[b] > 0) { sounding++; break; }
            nv++;
            voices++;
        } else if (c == 'F') {
            int16_t oa[NUM_SAMPLES_PER_FRAME * 6], ob[NUM_SAMPLES_PER_FRAME * 6];
            struct apu_bus_rt *rt = NULL;
            if (fread(R, sizeof(R), 1, f) != 1) break;
            memset(A, 0, sizeof(A));
            memset(B, 0, sizeof(B));
            t0 = now_us();
            for (int i = 0; i < nv; i++) old_mix(A, &vv[i]);
            t_old += now_us() - t0;
            if (memcmp(A, R, sizeof(A))) bad_game++;
            /* The cost: the same frame through the mixer, neutral and
             * with everything on (5.1), the sums and the output stage. */
            for (int pass = 0; pass < 2; pass++) {
                apu_bus_set_config(pass ? &busy : &cfg);
                apu_bus_enable(1);
                memset(B, 0, sizeof(B));
                t0 = now_us();
                rt = apu_bus_frame_begin(6, k_map);
                for (int i = 0; i < nv; i++)
                    apu_bus_mix_voice(rt, (int)((unsigned)vv[i].bin[5] + (unsigned)i) % 13, B,
                                      (const float (*)[2])vv[i].samples, vv[i].channels,
                                      vv[i].bin, vv[i].g);
                apu_bus_frame_end(rt, B);
                for (int ch = 0; ch < 6; ch++) memcpy(feeds[ch], B[ch], sizeof(feeds[ch]));
                apu_bus_output(feeds, 6, NUM_SAMPLES_PER_FRAME);
                if (pass) t_busy += now_us() - t0; else t_neutral += now_us() - t0;
            }
            apu_bus_set_config(&cfg);
            for (int six = 0; six < 2; six++) {
                apu_bus_enable(1);
                rt = apu_bus_frame_begin(six ? 6 : 2, k_map);
                memset(B, 0, sizeof(B));
                for (int i = 0; i < nv; i++)
                    apu_bus_mix_voice(rt, (int)((frames + (unsigned)i) % 13), B,
                                      (const float (*)[2])vv[i].samples, vv[i].channels,
                                      vv[i].bin, vv[i].g);
                apu_bus_frame_end(rt, B);
                if (memcmp(A, B, sizeof(A))) bad_bins++;
                dsp_out(B, six, ob);
                apu_bus_enable(0);
                dsp_out(A, six, oa);
                if (memcmp(oa, ob, sizeof(int16_t) * (six ? 6 : 2) * NUM_SAMPLES_PER_FRAME)) {
                    if (six) bad_51++; else bad_st++;
                }
                for (int i = 0; i < NUM_SAMPLES_PER_FRAME * (six ? 6 : 2); i++)
                    if (abs(oa[i]) > peak) peak = abs(oa[i]);
                {
                    /* The safety limiter (apu_limit.h) against the clamp
                     * alone, which is the output before it existed: a frame
                     * under the ceiling must be the same bytes, one over it
                     * must come out at or under. */
                    int16_t o0[NUM_SAMPLES_PER_FRAME * 6];
                    const int n = NUM_SAMPLES_PER_FRAME * (six ? 6 : 2);
                    int over = 0, still = 0;
                    dsp_out_lim(A, six, o0, 0);
                    for (int i = 0; i < n; i++)
                        if (abs(o0[i]) > LIM_CEIL16) over = 1;
                    if (six) {
                        /* A 5.1 title on a stereo device: today's six feeds
                         * (the clamp alone) folded (apu_spk_downmix), the
                         * saturating fold against the limited one. */
                        int16_t f0[NUM_SAMPLES_PER_FRAME][2], f1[NUM_SAMPLES_PER_FRAME][2];
                        int fover = 0, fhot = 0;
                        apu_limit_reset(); apu_limit_enable(0);
                        apu_spk_downmix((const int16_t (*)[APU_SPK_CHANNELS])o0, f0, NUM_SAMPLES_PER_FRAME);
                        apu_limit_reset(); apu_limit_enable(1);
                        apu_spk_downmix((const int16_t (*)[APU_SPK_CHANNELS])o0, f1, NUM_SAMPLES_PER_FRAME);
                        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
                            for (int c = 0; c < 2; c++) {
                                if (abs(f0[i][c]) > LIM_CEIL16) fover = 1;
                                if (abs(f0[i][c]) > fold_peak) fold_peak = abs(f0[i][c]);
                                if (abs(f1[i][c]) > LIM_CEIL16) fhot = 1;
                            }
                        if (!fover) { fold_quiet++; if (memcmp(f0, f1, sizeof f0)) fold_quiet_bad++; }
                        else { fold_loud++; fold_loud_bad += fhot; }
                    }
                    if (!over) {
                        lim_quiet++;
                        if (memcmp(oa, o0, sizeof(int16_t) * (size_t)n)) lim_quiet_bad++;
                    } else {
                        lim_loud++;
                        for (int i = 0; i < n; i++)
                            if (abs(oa[i]) > LIM_CEIL16) still = 1;
                        lim_loud_bad += still;
                    }
                }
            }
            frames++;
            nv = 0;
        } else {
            fprintf(stderr, "bad record tag %d\n", c);
            return 2;
        }
    }
    printf("%lu frames (%.1f s), %lu voice frames (%lu sounding), output peak %.0f of 32767\n",
           frames, frames / 1500.0, voices, sounding, peak);
    printf("frames where the loop's mixbins differ from the game's own:      %lu\n", bad_game);
    printf("frames where the mixer's mixbins (on, neutral) differ from it:   %lu\n", bad_bins);
    printf("frames where the s16 output differs, mixer off vs on, stereo:    %lu\n", bad_st);
    printf("frames where the s16 output differs, mixer off vs on, 5.1:       %lu\n", bad_51);
    printf("safety limiter against the clamp alone (stereo and 5.1 frames): %lu under the ceiling, of which differ: %lu;"
           " %lu over it, of which still above it: %lu\n", lim_quiet, lim_quiet_bad, lim_loud, lim_loud_bad);
    printf("fold of the 5.1 feeds to stereo (a 5.1 title on a stereo device), limiter against saturation: %lu frames under the"
           " ceiling, of which differ: %lu; %lu over it (fold peak %d of 32767), of which still above it: %lu\n",
           fold_quiet, fold_quiet_bad, fold_loud, fold_peak, fold_loud_bad);
    printf("cost a VP frame (32 samples, %.0f voices): the loop alone %.2f us, mixer neutral %.2f us,"
           " mixer with everything on %.2f us; a second of audio is 1500 frames: %.2f%% / %.2f%% / %.2f%% of a core\n",
           (double)voices / frames, t_old / frames, t_neutral / frames, t_busy / frames,
           t_old / frames * 0.15, t_neutral / frames * 0.15, t_busy / frames * 0.15);
    return frames && !bad_game && !bad_bins && !bad_st && !bad_51 && !lim_quiet_bad && !lim_loud_bad && !fold_quiet_bad && !fold_loud_bad ? 0 : 1;
}

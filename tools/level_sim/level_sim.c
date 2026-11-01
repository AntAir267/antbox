/*
 * level_sim.c -- the custom-music level (src/apu/apu_level.c) run offline
 * over decoded songs, as the game runs it on a custom-soundtrack voice
 * (BO-246). The code is the runtime's own; only the input is a file.
 *
 *   level_sim [--old] [--target dB] [--first] [--start dB] [--start-run LUFS]
 *             [--out out.f32] [--outdir dir] [--trace trace.tsv] song.f32...
 *
 * Each input is 48 kHz stereo float (ffmpeg -ar 48000 -ac 2 -f f32le), the
 * rate the APU's voice processor runs at. Songs play one after another as
 * the title plays a soundtrack: the first starts with no history (the -6 dB
 * start), each next one at the gain (and running level) the last one had
 * (--first: every song as a first song; --start dB / --start-run LUFS: every
 * song starts there). --old: the first leveller (BO-132). --target is the
 * target plus the mode's offset (default: louder, +4 dB). --out writes the
 * levelled samples, concatenated, for a loudness meter (--outdir: one file
 * a song, of the same name, in that folder); --trace every 100 ms
 * of the chain (song, s, short-term, running, gain, compressor).
 *
 * Per song: the gain at the end, the time-weighted mean of the gain, its
 * lowest and highest after the first 30 s (pumping), the share of frames
 * the gain was held (gated), the compressor's mean and deepest gain
 * reduction, and the time the chain took per 32-sample block.
 *
 * cc -O2 -I../../src/apu -I../../src/platform -I../../src level_sim.c ../../src/apu/apu_level.c -lm
 */
#include "apu_level.h"
#include "recomp_env.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* apu_level.c reads two variables through recomp_env.h and the limiter's
 * counter for its log; offline there are none. */
const char *recomp_env_value_[RENV_COUNT];
volatile int recomp_env_ready_ = 1;
void recomp_env_init(void) {}
unsigned long apu_limit_engaged(void) { return 0; }

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int old = 0, first_each = 0, have_start = 0, have_run = 0;
    float target = 1000.0f, start_db = 0.0f, start_run = -200.0f;
    float last = -1000.0f, last_run = -200.0f;
    double busy = 0.0;
    unsigned long busy_frames = 0;
    FILE *out = NULL, *trace = NULL;
    const char *outdir = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] == '-'; i++) {
        if (!strcmp(argv[i], "--target") && i + 1 < argc) target = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--old")) old = 1;
        else if (!strcmp(argv[i], "--first")) first_each = 1;
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) { have_start = 1; start_db = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--start-run") && i + 1 < argc) { have_run = 1; start_run = (float)atof(argv[++i]); }
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = fopen(argv[++i], "wb");
        else if (!strcmp(argv[i], "--outdir") && i + 1 < argc) outdir = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = fopen(argv[++i], "w");
        else { fprintf(stderr, "usage: see the head of level_sim.c\n"); return 2; }
    }
    if (target > 999.0f)
        target = (old ? APU_LEVEL_OLD_TARGET_DB : APU_LEVEL_TARGET_DB) + apu_level_mode_offset_db(APU_LEVEL_LOUDER);
    printf("%-7s %-7s %-7s %-7s %-6s %-6s %-6s %-8s %s\n", "end_dB", "mean_dB", "lo30", "hi30", "held%",
           "cmpAvg", "cmpMax", "ns/blk", "song");
    for (; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); continue; }
        apu_level_state st;
        apu_level_state_init(&st, first_each ? -1000.0f : have_start ? start_db : last);
        if (!first_each) {
            st.run_db = st.prior_db = have_run ? start_run : last_run;
        }
        static float blk[4096][32][2];
        unsigned long n = 0, held = 0;
        double sum_g = 0.0;
        float lo = 1000.0f, hi = -1000.0f;
        size_t got;
        const char *name = strrchr(argv[i], '/');
        name = name ? name + 1 : argv[i];
        FILE *song_out = NULL;
        if (outdir) {
            char p[4096];
            snprintf(p, sizeof(p), "%s/%s", outdir, name);
            song_out = fopen(p, "wb");
        }
        while ((got = fread(blk, sizeof(blk[0]), 4096, f)) > 0) {
            for (size_t k = 0; k < got; k++) {
                const double t0 = now_s();
                if (old) apu_level_frame_old(&st, blk[k], 2, target);
                else apu_level_frame(&st, blk[k], 2, target);
                busy += now_s() - t0;
                busy_frames++;
                n++;
                sum_g += st.gain_db;
                if (st.hold) held++;
                if (n > 30 * 1500) {
                    if (st.gain_db < lo) lo = st.gain_db;
                    if (st.gain_db > hi) hi = st.gain_db;
                }
                if (trace && n % 150 == 0)
                    fprintf(trace, "%s\t%.1f\t%.2f\t%.2f\t%.2f\t%.2f\n", name, n / 1500.0, st.st_db, st.run_db,
                            st.gain_db, -st.comp_db);
            }
            if (out) fwrite(blk, sizeof(blk[0]), got, out);
            if (song_out) fwrite(blk, sizeof(blk[0]), got, song_out);
        }
        fclose(f);
        if (song_out) fclose(song_out);
        if (!n) continue;
        if (st.primed) { last = st.gain_db; last_run = st.run_db; }
        printf("%+7.2f %+7.2f %+7.2f %+7.2f %6.1f %6.2f %6.2f %8.0f %s\n", st.gain_db, sum_g / n,
               lo < 999 ? lo : 0, hi > -999 ? hi : 0, 100.0 * held / n,
               old ? 0.0 : st.comp_sum_db / st.frames, old ? 0.0 : st.comp_max_db,
               busy_frames ? busy / busy_frames * 1e9 : 0.0, name);
    }
    if (out) fclose(out);
    if (trace) fclose(trace);
    return 0;
}

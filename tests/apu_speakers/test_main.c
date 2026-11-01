/* apu_speakers: the 5.1 path's pure parts (BO-76).
 *
 *   - RECOMP_AUDIO_SPEAKERS parsing, and what auto/stereo/5.1 decide for
 *     the title and for the device at each device channel count
 *   - the dashboard word the title is told (XC_AUDIO)
 *   - the bin -> speaker map: DirectSound's layout by default, a title's
 *     map when given, and the conversion the stereo path uses
 *   - the ITU-R BS.775 fold of 5.1 to stereo
 *   - the WAV tap at six channels
 *
 *   apu_speakers_test <scratch.wav>
 */
#include "apu_speakers.h"
#include "apu_limit.h"
#include "apu_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

#define N 32

static float bins[APU_SPK_BINS][N];

/* What apu_dsp.c's stereo fold makes of bins: even bins left, odd right,
 * clamp, * 32767, truncate. The reference the 5.1 path's conversion must
 * agree with. */
static void stereo_fold(int16_t out[N][2])
{
    for (int i = 0; i < N; i++) {
        float l = 0, r = 0;
        for (int b = 0; b < APU_SPK_BINS; b++) {
            if (b & 1) r += bins[b][i];
            else       l += bins[b][i];
        }
        if (l > 1.0f) l = 1.0f;
        if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f;
        if (r < -1.0f) r = -1.0f;
        out[i][0] = (int16_t)(l * 32767.0f);
        out[i][1] = (int16_t)(r * 32767.0f);
    }
}

static void test_parse_and_decide(void)
{
    CHECK("parse NULL = auto", apu_spk_parse(NULL) == APU_SPK_AUTO);
    CHECK("parse empty = auto", apu_spk_parse("") == APU_SPK_AUTO);
    CHECK("parse auto", apu_spk_parse("auto") == APU_SPK_AUTO);
    CHECK("parse stereo", apu_spk_parse("stereo") == APU_SPK_STEREO);
    CHECK("parse Stereo", apu_spk_parse("Stereo") == APU_SPK_STEREO);
    CHECK("parse 5.1", apu_spk_parse("5.1") == APU_SPK_51);
    CHECK("parse surround", apu_spk_parse("surround") == APU_SPK_51);
    CHECK("parse junk = auto", apu_spk_parse("7.1") == APU_SPK_AUTO);

    /* title channels: auto follows the device, the others do not */
    CHECK("auto, device unknown -> stereo", apu_spk_title_channels(APU_SPK_AUTO, 0) == 2);
    CHECK("auto, device 2 -> stereo", apu_spk_title_channels(APU_SPK_AUTO, 2) == 2);
    CHECK("auto, device 4 -> stereo", apu_spk_title_channels(APU_SPK_AUTO, 4) == 2);
    CHECK("auto, device 6 -> 5.1", apu_spk_title_channels(APU_SPK_AUTO, 6) == 6);
    CHECK("auto, device 8 -> 5.1", apu_spk_title_channels(APU_SPK_AUTO, 8) == 6);
    CHECK("stereo, device 8 -> stereo", apu_spk_title_channels(APU_SPK_STEREO, 8) == 2);
    CHECK("5.1, device 2 -> 5.1", apu_spk_title_channels(APU_SPK_51, 2) == 6);
    CHECK("5.1, device unknown -> 5.1", apu_spk_title_channels(APU_SPK_51, 0) == 6);

    /* device channels: a stereo title opens stereo; a 5.1 title opens six
     * unless the device is known to have fewer (then we fold) */
    CHECK("stereo title -> 2", apu_spk_device_channels(2, 6) == 2);
    CHECK("5.1 title, 6 -> 6", apu_spk_device_channels(6, 6) == 6);
    CHECK("5.1 title, 8 -> 6", apu_spk_device_channels(6, 8) == 6);
    CHECK("5.1 title, 2 -> 2 (fold)", apu_spk_device_channels(6, 2) == 2);
    CHECK("5.1 title, unknown -> 6", apu_spk_device_channels(6, 0) == 6);

    CHECK("XC_AUDIO stereo 0", apu_spk_xc_audio(2) == 0);
    CHECK("XC_AUDIO 5.1 = SURROUND|AC3", apu_spk_xc_audio(6) == 0x00010002u);
}

/* One bin at a time carries a ramp; exactly its speaker must get it. */
static void test_map(const apu_spk_map map, const char *what)
{
    int16_t out[N][APU_SPK_CHANNELS];
    char name[128];

    for (int b = 0; b < APU_SPK_BINS; b++) {
        memset(bins, 0, sizeof(bins));
        for (int i = 0; i < N; i++)
            bins[b][i] = (i + 1) / 64.0f;
        apu_spk_mix51(&bins[0][0], N, map, NULL, out, N);
        for (int c = 0; c < APU_SPK_CHANNELS; c++) {
            int want = map[b] == c, got = 1;
            for (int i = 0; i < N; i++) {
                int16_t e = want ? (int16_t)((i + 1) / 64.0f * 32767.0f) : 0;
                if (out[i][c] != e)
                    got = 0;
            }
            snprintf(name, sizeof name, "%s: bin %d channel %d", what, b, c);
            CHECK(name, got);
        }
    }
}

static void test_default_map(void)
{
    apu_spk_map m;
    apu_spk_default_map(m);
    for (int b = 0; b < 6; b++)
        CHECK("speaker bins are themselves", m[b] == b);
    CHECK("XTLK FL", m[6] == APU_SPK_FL);
    CHECK("XTLK FR", m[7] == APU_SPK_FR);
    CHECK("XTLK BL", m[8] == APU_SPK_SL);
    CHECK("XTLK BR", m[9] == APU_SPK_SR);
    for (int b = 10; b < APU_SPK_BINS; b++)
        CHECK("sends: even left, odd right", m[b] == ((b & 1) ? APU_SPK_FR : APU_SPK_FL));
    test_map(m, "default map");
}

/* A title's map (Burnout 3's, as its main.c sets it): sends to surround
 * speakers, and a bin left out. */
static void test_title_map(void)
{
    static const int8_t bo3[32] = {
        0, 1, 2, 3, 4, 5, 0, 1, 4, 5, 0, 1, 0,
        1, 4, 5, 2, 0, 1, 0, 1, 4, 5, 2, 3, 0, -1, 1, 0, 1, 0, 1,
    };
    test_map(bo3, "title map");
}

/* The 5.1 path converts exactly as the stereo fold does: with only the
 * front pair's bins carrying sound (bins 0, 1 and the sends 10..31, which
 * the default map folds like the stereo path), FL/FR are the stereo path's
 * L/R sample for sample, clamping included. */
static void test_same_conversion(void)
{
    apu_spk_map m;
    int16_t six[N][APU_SPK_CHANNELS], two[N][2];
    int same = 1;

    apu_spk_default_map(m);
    srand(76);
    memset(bins, 0, sizeof(bins));
    for (int b = 0; b < APU_SPK_BINS; b++) {
        if (b >= 2 && b < 10)
            continue;
        for (int i = 0; i < N; i++)
            bins[b][i] = (float)(rand() % 2001 - 1000) / 4000.0f;   /* sums clip */
    }
    /* The clamp alone (the safety limiter, apu_limit.h, off): the
     * conversion this test is about. */
    apu_limit_enable(0);
    apu_spk_mix51(&bins[0][0], N, m, NULL, six, N);
    stereo_fold(two);
    for (int i = 0; i < N; i++)
        if (six[i][APU_SPK_FL] != two[i][0] || six[i][APU_SPK_FR] != two[i][1])
            same = 0;
    CHECK("front pair = stereo fold, bit for bit", same);
    /* With the limiter on, the same clipping sums come out at or under -0.5 dBFS. */
    apu_limit_enable(1);
    apu_limit_reset();
    apu_spk_mix51(&bins[0][0], N, m, NULL, six, N);
    {
        int under = 1, hot = 0;
        for (int i = 0; i < N; i++)
            for (int c = 0; c < 2; c++) {
                if (abs(six[i][c]) > 30934) under = 0;
                if (abs(two[i][c]) == 32767) hot = 1;
            }
        CHECK("the limiter keeps the clipping sums under the ceiling", under && hot);
    }
    for (int i = 0; i < N; i++)
        for (int c = APU_SPK_FC; c < APU_SPK_CHANNELS; c++)
            CHECK("the rest silent", six[i][c] == 0);
}

static void fold1(int16_t fl, int16_t fr, int16_t c, int16_t lfe, int16_t sl,
                  int16_t sr, int16_t *l, int16_t *r)
{
    int16_t in[1][APU_SPK_CHANNELS] = { { fl, fr, c, lfe, sl, sr } };
    int16_t out[1][2];
    apu_spk_downmix((const int16_t (*)[APU_SPK_CHANNELS])in, out, 1);
    *l = out[0][0];
    *r = out[0][1];
}

static void test_downmix(void)
{
    apu_limit_enable(0);        /* the fold and its saturation, as before the limiter (apu_limit.h) */
    int16_t l, r;
    double k = sqrt(0.5);

    fold1(10000, 0, 0, 0, 0, 0, &l, &r);
    CHECK("FL -> L only, unity", l == 10000 && r == 0);
    fold1(0, -10000, 0, 0, 0, 0, &l, &r);
    CHECK("FR -> R only, unity", l == 0 && r == -10000);
    fold1(0, 0, 10000, 0, 0, 0, &l, &r);
    CHECK("C -> both at -3 dB", abs(l - (int)(10000 * k)) <= 1 && l == r);
    fold1(0, 0, 0, 0, 10000, 0, &l, &r);
    CHECK("SL -> L at -3 dB", abs(l - (int)(10000 * k)) <= 1 && r == 0);
    fold1(0, 0, 0, 0, 0, 10000, &l, &r);
    CHECK("SR -> R at -3 dB", l == 0 && abs(r - (int)(10000 * k)) <= 1);
    fold1(0, 0, 0, 30000, 0, 0, &l, &r);
    CHECK("LFE left out", l == 0 && r == 0);
    fold1(1000, 2000, 3000, 4000, 5000, 6000, &l, &r);
    CHECK("sum L", abs(l - (int)(1000 + k * 3000 + k * 5000)) <= 1);
    CHECK("sum R", abs(r - (int)(2000 + k * 3000 + k * 6000)) <= 1);
    fold1(32767, 32767, 32767, 0, 32767, 32767, &l, &r);
    CHECK("full scale saturates high, no wrap", l == 32767 && r == 32767);
    fold1(-32768, -32768, -32768, 0, -32768, -32768, &l, &r);
    CHECK("full scale saturates low, no wrap", l == -32768 && r == -32768);
}

/* The limiter on the fold of a 5.1 title played on a stereo device: under
 * the ceiling not a bit moves; over it the fold comes out at or under
 * -0.5 dBFS (30934) instead of saturating. */
static void test_downmix_limiter(void)
{
    enum { B = 256 };
    static int16_t in[B][APU_SPK_CHANNELS], on[B][2], off[B][2];
    unsigned long quiet = 0, quiet_bad = 0, loud = 0, loud_bad = 0;
    int peak_fold = 0;

    srand(5);
    for (int pass = 0; pass < 3; pass++) {
        const int amp = pass == 0 ? 32767 : pass == 1 ? 6000 : 2500;
        for (int blk = 0; blk < 400; blk++) {
            int over = 0;
            for (int i = 0; i < B; i++)
                for (int c = 0; c < APU_SPK_CHANNELS; c++)
                    in[i][c] = (int16_t)(rand() % (2 * amp + 1) - amp);
            apu_limit_reset();
            apu_limit_enable(0);
            apu_spk_downmix((const int16_t (*)[APU_SPK_CHANNELS])in, off, B);
            apu_limit_reset();
            apu_limit_enable(1);
            apu_spk_downmix((const int16_t (*)[APU_SPK_CHANNELS])in, on, B);
            for (int i = 0; i < B; i++)
                for (int c = 0; c < 2; c++) {
                    if (abs(off[i][c]) > 30934) over = 1;
                    if (abs(off[i][c]) > peak_fold) peak_fold = abs(off[i][c]);
                }
            if (!over) {
                quiet++;
                if (memcmp(on, off, sizeof on)) quiet_bad++;
            } else {
                int hot = 0;
                loud++;
                for (int i = 0; i < B; i++)
                    for (int c = 0; c < 2; c++)
                        if (abs(on[i][c]) > 30934) hot = 1;
                loud_bad += hot;
            }
        }
    }
    CHECK("fold limiter: blocks under the ceiling exist", quiet > 100);
    CHECK("fold limiter: blocks over the ceiling exist", loud > 100);
    CHECK("fold limiter: under the ceiling, not a bit differs from the saturating fold", quiet_bad == 0);
    CHECK("fold limiter: over it, never above -0.5 dBFS", loud_bad == 0);
    /* The measured case: a crash with the fold at +3 dBFS (sum 1.41 x). */
    apu_limit_reset();
    apu_limit_enable(1);
    {
        int16_t l, r, pk = 0;
        for (int i = 0; i < 480; i++) {
            fold1(28000, 28000, 14000, 0, 14000, 14000, &l, &r);   /* 28000 + 0.707 * 28000 = 47800: +3.3 dBFS */
            if (abs(l) > pk) pk = (int16_t)abs(l);
        }
        CHECK("fold limiter: +3.3 dBFS fold held at the ceiling", pk <= 30934 && pk > 30000);
    }
    apu_limit_reset();
    printf("fold limiter: %lu blocks under the ceiling (%lu differ), %lu over (%lu above -0.5 dBFS)\n",
           quiet, quiet_bad, loud, loud_bad);
}

static uint32_t le32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void test_wav6(const char *path)
{
    int16_t g[100][6];
    unsigned char h[44];
    FILE *f;
    int ok = 1;

    for (int i = 0; i < 100; i++)
        for (int c = 0; c < 6; c++)
            g[i][c] = (int16_t)(i * 6 + c - 300);
    apu_wav_set_channels(6);
    CHECK("wav open", apu_wav_open(path, 0));
    apu_wav_set_channels(2);                 /* ignored while open */
    apu_wav_write(&g[0][0], 100);
    apu_wav_close();
    f = fopen(path, "rb");
    if (!f) { CHECK("wav reopen", 0); return; }
    CHECK("wav header", fread(h, 1, 44, f) == 44);
    CHECK("6 channels", h[22] == 6);
    CHECK("block align 12", h[32] == 12);
    CHECK("byte rate 576000", le32(h + 28) == 48000u * 12);
    CHECK("data 100 frames", le32(h + 40) == 1200);
    for (int i = 0; i < 600 && ok; i++) {
        unsigned char b[2];
        if (fread(b, 1, 2, f) != 2 || (int16_t)(b[0] | (b[1] << 8)) != (&g[0][0])[i])
            ok = 0;
    }
    CHECK("wav payload interleaved FL FR FC LFE SL SR", ok);
    fclose(f);
}

int main(int argc, char **argv)
{
    test_parse_and_decide();
    test_default_map();
    test_title_map();
    test_same_conversion();
    test_downmix();
    test_downmix_limiter();
    if (argc > 1)
        test_wav6(argv[1]);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("apu_speakers: all passed\n");
    return 0;
}

/* soundtrack: the music-folder scan, the ST.DB image as the XDK library
 * reads it, and the decoders' output (xbox_soundtrack.h).
 *
 *   soundtrack_test <work dir> <fixture dir>
 *
 * The WAV files are written here; the encoded fixtures (data/, half a second
 * of a 1 kHz sine each) come from make_fixtures.sh. */
#include "xbox_soundtrack.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if !defined(_WIN32)
#include <unistd.h>      /* symlink */
#endif
#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
/* Paths are UTF-8 in this test as in the library: the wide calls. */
static int u8_mkdir(const char *path)
{
    wchar_t w[1024];
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) > 0 ? _wmkdir(w) : -1;
}
static FILE *u8_fopen(const char *path, const char *mode)
{
    wchar_t w[1024], m[8];
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024) <= 0
        || MultiByteToWideChar(CP_UTF8, 0, mode, -1, m, 8) <= 0)
        return NULL;
    return _wfopen(w, m);
}
#define mkdir(p, m) u8_mkdir(p)
#else
#define u8_fopen fopen
#endif

static int failures;
#define CHECK(what, cond) do { \
    if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, what); failures++; } \
} while (0)

static char g_work[1024], g_data[1024];

static const char *in_work(const char *rel)
{
    static char p[4][1400];
    static int k;
    char *o = p[k++ & 3];
    snprintf(o, sizeof p[0], "%s/%s", g_work, rel);
    return o;
}

static void put(FILE *f, uint32_t v, int n) { while (n--) { fputc((int)(v & 0xFF), f); v >>= 8; } }

/* A 16-bit WAV: a sine of `hz` at half scale, `ms` long. */
static void write_wav(const char *path, unsigned rate, unsigned ch, double hz, unsigned ms)
{
    FILE *f = u8_fopen(path, "wb");
    uint32_t n = rate * ms / 1000, i;
    unsigned c;

    if (!f) { CHECK("write_wav", 0); return; }
    fwrite("RIFF", 1, 4, f); put(f, 36 + n * ch * 2, 4); fwrite("WAVEfmt ", 1, 8, f);
    put(f, 16, 4); put(f, 1, 2); put(f, ch, 2); put(f, rate, 4); put(f, rate * ch * 2, 4);
    put(f, ch * 2, 2); put(f, 16, 2); fwrite("data", 1, 4, f); put(f, n * ch * 2, 4);
    for (i = 0; i < n; i++)
        for (c = 0; c < ch; c++)
            put(f, (uint32_t)(int16_t)lrint(16384.0 * sin(2 * 3.14159265358979323846 * hz * i / rate)), 2);
    fclose(f);
}

static void copy_file(const char *from, const char *to)
{
    FILE *a = u8_fopen(from, "rb"), *b = u8_fopen(to, "wb");
    char buf[4096];
    size_t n;
    if (!a || !b) { printf("copy %s -> %s\n", from, to); CHECK("copy_file", 0); }
    while (a && b && (n = fread(buf, 1, sizeof buf, a)) > 0)
        fwrite(buf, 1, n, b);
    if (a) fclose(a);
    if (b) fclose(b);
}

static void fixture(const char *name, const char *to_rel)
{
    char from[1400];
    snprintf(from, sizeof from, "%s/%s", g_data, name);
    copy_file(from, in_work(to_rel));
}

static int name_is(const uint16_t *name, const char *ascii)
{
    int i;
    for (i = 0; ascii[i]; i++)
        if (name[i] != (unsigned char)ascii[i])
            return 0;
    return name[i] == 0;
}

/* The share of a signal's power at `hz` (Goertzel), 0..1. */
static double tone_share(const int16_t *pcm, size_t frames, int ch, double hz)
{
    double w = 2 * 3.14159265358979323846 * hz / XST_RATE, c = 2 * cos(w);
    double s0, s1 = 0, s2 = 0, total = 0;
    size_t i;
    for (i = 0; i < frames; i++) {
        double x = pcm[i * 2 + ch];
        s0 = x + c * s1 - s2; s2 = s1; s1 = s0;
        total += x * x;
    }
    if (total == 0 || frames == 0)
        return 0;
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / (total * frames / 2.0);
}

static double rms_db(const int16_t *pcm, size_t frames)
{
    double sum = 0;
    size_t i;
    for (i = 0; i < frames * 2; i++)
        sum += (double)pcm[i] * pcm[i];
    return 10 * log10(sum / (frames * 2) / (32768.0 * 32768.0) + 1e-20);
}

static size_t decode_all(const char *path, int16_t *out, size_t cap, unsigned *rate, unsigned *ch)
{
    xst_stream *s = xst_stream_open(path);
    size_t n = 0, got;
    if (!s)
        return 0;
    xst_stream_format(s, rate, ch);
    /* Odd-sized reads, as a caller with its own packet size would make. */
    while (n + 1000 <= cap && (got = xst_stream_read(s, out + n * 2, 1000)) > 0)
        n += got;
    CHECK("a finished stream stays finished", xst_stream_read(s, out, 16) == 0);
    xst_stream_close(s);
    return n;
}

/* ── tests ─────────────────────────────────────────────────────────── */

static void test_names(void)
{
    uint16_t n[XST_NAME_UNITS];

    CHECK("2 before 10", xst_natural_cmp("2 song.mp3", "10 song.mp3") < 0);
    CHECK("case-insensitive", xst_natural_cmp("b.mp3", "A.mp3") > 0);
    CHECK("leading zeros", xst_natural_cmp("02 x", "2 y") < 0);
    CHECK("prefix first", xst_natural_cmp("Track", "Track 2") < 0);
    CHECK("equal", xst_natural_cmp("a1", "a1") == 0);
    CHECK("long digit runs", xst_natural_cmp("20260101120000", "20260101120001") < 0);

    xst_name_from_utf8(n, "\xC3\x81rv\xC3\xADzt\xC5\xB1r\xC5\x91");      /* Árvíztűrő */
    /* Latin-1 as it is; the double acutes, which a menu font does not have,
     * as the diaeresis. */
    CHECK("utf-8 to utf-16", n[0] == 0xC1 && n[3] == 0xED && n[6] == 0xFC && n[8] == 0xF6 && n[9] == 0);
    xst_name_from_utf8(n, "A\xCC\x81rvi\xCC\x81ztu\xCC\x8Bro\xCC\x8B n\xCC\x83 z\xCC\x8C");  /* decomposed */
    CHECK("decomposed letters composed", n[0] == 0xC1 && n[1] == 'r' && n[3] == 0xED && n[4] == 'z'
                                         && n[6] == 0xFC && n[8] == 0xF6 && n[10] == 0xF1
                                         && n[12] == 'z' && n[13] == 0);
    /* "Rip" with a caron and an acute, curly quotes, an en dash, a Cyrillic letter. */
    xst_name_from_utf8(n, "\xC5\x98\xC3\xADp \xE2\x80\x9Cx\xE2\x80\x9D \xE2\x80\x93 \xD0\x96");
    CHECK("folded to Latin-1", n[0] == 'R' && n[1] == 0xED && n[4] == '"' && n[6] == '"'
                               && n[8] == '-' && n[10] == '?' && n[11] == 0);
    xst_name_from_utf8(n, "0123456789012345678901234567890123456789");
    CHECK("cut at 31 units", n[30] == '0' && n[31] == 0);
    xst_name_from_utf8(n, "a\xF0\x9F\x8E\xB5" "b");                      /* outside the BMP */
    CHECK("one unit a character", n[0] == 'a' && n[1] == '?' && n[2] == 'b' && n[3] == 0);
    xst_name_from_utf8(n, "bad\xFF");
    CHECK("invalid byte", n[3] == '?' && n[4] == 0);

    CHECK("mp3", xst_is_playable_name("a.MP3"));
    CHECK("flac", xst_is_playable_name("a.b.flac"));
#if defined(__APPLE__)
    CHECK("m4a where the system decodes it", xst_is_playable_name("a.m4a"));
#else
    CHECK("not m4a", !xst_is_playable_name("a.m4a"));
#endif
    CHECK("not wma", !xst_is_playable_name("a.wma"));
    CHECK("not hidden", !xst_is_playable_name("._a.mp3"));
    CHECK("no extension", !xst_is_playable_name("mp3"));
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* XGetSoundtrackSongInfo as the XDK library does it: the soundtrack's block
 * by its place in the header's id list, the song's group by index / 6. */
static int db_song(const uint8_t *db, size_t size, uint32_t st_id, uint32_t index,
                   uint32_t *id, uint32_t *ms, uint16_t *name)
{
    uint32_t n = rd32(db + 4), i, gid;
    const uint8_t *st, *g;
    int k;

    if (rd32(db) != 1)
        return 0;
    for (i = 0; i < n && i < 100; i++)
        if (rd32(db + 12 + i * 4) == st_id)
            break;
    if (i == n)
        return 0;
    st = db + 512 * (1 + i);
    if (rd32(st) != 0x21371 || rd32(st + 4) != st_id || index >= rd32(st + 8))
        return 0;
    gid = rd32(st + 12 + (index / 6) * 4);
    if ((size_t)(gid + 0x65 + 1) * 512 > size)
        return 0;
    g = db + (size_t)(gid + 0x65) * 512;
    if (rd32(g) != 0x31073 || rd32(g + 4) != st_id)
        return 0;
    *id = rd32(g + 16 + (index % 6) * 4);
    *ms = rd32(g + 40 + (index % 6) * 4);
    for (k = 0; k < 32; k++)
        name[k] = (uint16_t)(g[64 + (index % 6) * 64 + k * 2] | g[64 + (index % 6) * 64 + k * 2 + 1] << 8);
    return 1;
}

static void test_scan_and_db(void)
{
    xst_library *lib;
    uint8_t *db;
    size_t size;
    char rel[64];
    int i;

    mkdir(in_work("lib"), 0755);
    mkdir(in_work("lib/B album"), 0755);
    mkdir(in_work("lib/a first"), 0755);
    mkdir(in_work("lib/a second"), 0755);
    /* An album layout: counted into "a first", not soundtracks of their own;
     * three levels below "a first" is not read. */
    mkdir(in_work("lib/a first/album"), 0755);
    mkdir(in_work("lib/a first/album/disc 1"), 0755);
    mkdir(in_work("lib/a first/album/disc 1/too deep"), 0755);
    mkdir(in_work("elsewhere"), 0755);
    mkdir(in_work("lib/empty"), 0755);
    mkdir(in_work("lib/.hidden"), 0755);
    write_wav(in_work("lib/10 ten.wav"), 44100, 2, 440, 300);
    write_wav(in_work("lib/2 two.wav"), 44100, 1, 440, 200);
    write_wav(in_work("lib/._2 two.wav"), 44100, 1, 440, 200);
    write_wav(in_work("lib/.hidden/x.wav"), 44100, 1, 440, 200);
    copy_file(in_work("lib/2 two.wav"), in_work("lib/notes.txt"));
    copy_file(in_work("lib/notes.txt"), in_work("lib/broken.flac"));        /* not a FLAC */
    fixture("tone_id3v23.mp3", "lib/a first/01.mp3");
    fixture("tone.flac", "lib/a first/02.flac");
    fixture("tone48.ogg", "lib/a second/a.ogg");
    write_wav(in_work("lib/a first/album/1 in album.wav"), 44100, 1, 440, 200);
    write_wav(in_work("lib/a first/album/disc 1/1 on disc.wav"), 44100, 1, 440, 200);
    write_wav(in_work("lib/a first/album/disc 1/too deep/deep.wav"), 44100, 1, 440, 200);
    write_wav(in_work("elsewhere/linked.wav"), 44100, 1, 440, 200);
#if !defined(_WIN32)
    /* A folder that is a link is not followed (a link to / would be the
     * whole disk). */
    if (symlink(in_work("elsewhere"), in_work("lib/z link")) != 0)
        CHECK("symlink", 0);
#endif
    for (i = 1; i <= 14; i++) {                                             /* three groups */
        snprintf(rel, sizeof rel, "lib/B album/%d.wav", i);
        write_wav(in_work(rel), 22050, 1, 440, 100 * (unsigned)i);
    }

    lib = xst_scan(in_work("lib"));
    CHECK("scan", lib != NULL);
    if (!lib)
        return;
    CHECK("four soundtracks", lib->n_st == 4);
    CHECK("songs (none from too deep, none through the link)", lib->n_songs == 2 + 4 + 1 + 14);
    CHECK("the album and its disc counted into 'a first', in order",
          lib->st[1].count == 4 && name_is(lib->songs[4].name, "1 in album")
          && name_is(lib->songs[5].name, "1 on disc"));
    CHECK("the whole folder was read", !lib->budget_hit);
    CHECK("the broken file is counted", lib->bad_files == 1);
    /* The folder itself first, then its folders in natural order, each
     * followed by its own. */
    CHECK("root named after the folder", name_is(lib->st[0].name, "lib"));
    CHECK("then 'a first'", name_is(lib->st[1].name, "a first"));
    CHECK("then 'a second' (a folder in it, not below one)", name_is(lib->st[2].name, "a second"));
    CHECK("then 'B album'", name_is(lib->st[3].name, "B album"));
    CHECK("ids from 1", lib->st[0].id == 1 && lib->st[3].id == 4);
    {
        /* BO-230: a choice is remembered by name and found again in a list that changed */
        char nm[64];
        CHECK("name of 2", xst_soundtrack_name(lib, 2, nm, sizeof nm) && !strcmp(nm, "a second"));
        CHECK("name of 0", xst_soundtrack_name(lib, 0, nm, sizeof nm) && !strcmp(nm, "lib"));
        CHECK("no name past the end", !xst_soundtrack_name(lib, 4, nm, sizeof nm) && !nm[0]);
        CHECK("name cut to the buffer", xst_soundtrack_name(lib, 2, nm, 4) && !strcmp(nm, "a s"));
        CHECK("find by name", xst_soundtrack_find(lib, "B album") == 3 && xst_soundtrack_find(lib, "a first") == 1);
        CHECK("find is not found", xst_soundtrack_find(lib, "nope") == -1 && xst_soundtrack_find(lib, "") == -1
                                   && xst_soundtrack_find(lib, NULL) == -1);
        CHECK("find on no library", xst_soundtrack_find(NULL, "lib") == -1);
    }
    CHECK("2 before 10", name_is(lib->songs[0].name, "2 two") && name_is(lib->songs[1].name, "10 ten"));
    CHECK("wav length", lib->songs[0].ms == 200 && lib->songs[1].ms == 300);
    CHECK("soundtrack length", lib->st[0].ms == 500);
    /* Title tags: ID3v2.3 (UTF-16 there), a FLAC and an Ogg comment. */
    CHECK("id3 title", lib->songs[2].name[0] == 0xC1 && lib->songs[2].name[6] == 0xFC
                       && lib->songs[2].name[9] == ' ');
    CHECK("flac title", name_is(lib->songs[3].name, "Flac Title"));
    CHECK("ogg title", name_is(lib->songs[6].name, "Ogg Title"));
    CHECK("mp3 length about 0.5 s", lib->songs[2].ms > 400 && lib->songs[2].ms < 650);
    CHECK("flac length", lib->songs[3].ms == 500);
    CHECK("ogg length", lib->songs[6].ms >= 495 && lib->songs[6].ms <= 505);
    CHECK("song ids", lib->songs[0].id == 0x00010000 && lib->songs[1].id == 0x00010001
                      && lib->songs[5].id == 0x00020003 && lib->songs[7].id == 0x00040000);
    CHECK("by id", xst_song_by_id(lib, 0x00040003) == &lib->songs[7 + 3]);
    CHECK("no such song", !xst_song_by_id(lib, 0x00040010) && !xst_song_by_id(lib, 0x00000000)
                          && !xst_song_by_id(lib, 0x00050000));

    size = xst_db_size(lib);
    CHECK("db size: header, 100 soundtracks, 1+1+1+3 groups", size == 512 * (101 + 6));
    db = (uint8_t *)malloc(size);
    CHECK("too small a buffer", xst_db_build(lib, db, size - 1) == 0);
    CHECK("db build", xst_db_build(lib, db, size) == size);
    CHECK("header", rd32(db) == 1 && rd32(db + 4) == 4 && rd32(db + 12) == 1 && rd32(db + 24) == 4
                    && rd32(db + 28) == 0);
    CHECK("soundtrack block", rd32(db + 512 * 4) == 0x21371 && rd32(db + 512 * 4 + 8) == 14
                              && rd32(db + 512 * 4 + 348) == lib->st[3].ms
                              && db[512 * 4 + 352] == 'B' && db[512 * 4 + 353] == 0);
    CHECK("unused soundtrack blocks are empty", rd32(db + 512 * 5) == 0);
    for (i = 0; i < lib->n_st; i++) {
        int k;
        for (k = 0; k < lib->st[i].count; k++) {
            const xst_song *s = &lib->songs[lib->st[i].first + k];
            uint32_t id = 0, ms = 0;
            uint16_t name[32];
            int ok = db_song(db, size, lib->st[i].id, (uint32_t)k, &id, &ms, name);
            CHECK("song read back", ok && id == s->id && ms == s->ms
                                    && !memcmp(name, s->name, sizeof name));
            CHECK("song's folder is its soundtrack", (id >> 16) == lib->st[i].id);
        }
    }
    {
        uint32_t id, ms; uint16_t name[32];
        CHECK("index past the end", !db_song(db, size, 4, 14, &id, &ms, name));
        CHECK("unknown soundtrack", !db_song(db, size, 9, 0, &id, &ms, name));
    }
    free(db);
    xst_free(lib);

    CHECK("a folder that is not there", xst_scan(in_work("nothing here")) == NULL);
    lib = xst_scan(in_work("lib/empty"));
    CHECK("an empty folder", lib && lib->n_st == 0 && lib->n_songs == 0);
    xst_free(lib);
}

static void test_limits(void)
{
    xst_library *lib;
    char rel[64];
    int i;

    mkdir(in_work("many"), 0755);
    write_wav(in_work("many/1.wav"), 8000, 1, 440, 10);
    for (i = 2; i <= XST_MAX_SONGS + 3; i++) {
        snprintf(rel, sizeof rel, "many/%d.wav", i);
        copy_file(in_work("many/1.wav"), in_work(rel));
    }
    /* An ID3v2.3 tag whose extended-header size is corrupt (audit finding
     * 16): the frame walk must stop, not read past the tag. */
    {
        unsigned char t[10 + 100];
        uint32_t ms = 0;
        char title[64] = "";
        FILE *f = fopen(in_work("badext.mp3"), "wb");
        memset(t, 0, sizeof t);
        memcpy(t, "ID3\x03\x00\x40\x00\x00\x00\x64", 10);   /* v2.3, extended header, 100 bytes */
        t[10] = 0xFF; t[11] = 0xFF; t[12] = 0xFF; t[13] = 0xF0;     /* its size: near 4 GB */
        if (f) { fwrite(t, 1, sizeof t, f); fclose(f); }
        xst_probe(in_work("badext.mp3"), &ms, title, sizeof title);
        CHECK("a corrupt extended header: no title, no crash", title[0] == 0);
    }

    /* A folder of more entries than a scan reads (Home, a whole disk):
     * it stops there, says so, and offers what it read. */
    mkdir(in_work("huge"), 0755);
    for (i = 0; i < XST_SCAN_BUDGET + 50; i++) {
        FILE *f;
        snprintf(rel, sizeof rel, "huge/note %05d.txt", i);
        f = fopen(in_work(rel), "wb");
        if (f) fclose(f);
    }
    lib = xst_scan(in_work("huge"));
    CHECK("a folder past the budget: scanned, stopped, said", lib && lib->budget_hit && lib->n_songs == 0);
    xst_free(lib);

    lib = xst_scan(in_work("many"));
    CHECK("500 songs kept", lib && lib->n_songs == XST_MAX_SONGS && lib->dropped_songs == 3);
    if (lib) {
        CHECK("the first 500 in order", name_is(lib->songs[499].name, "500"));
        CHECK("84 groups", xst_db_size(lib) == 512 * (101 + 84));
    }
    xst_free(lib);
}

static void test_decode(void)
{
    static int16_t pcm[XST_RATE * 2 * 2];
    const size_t cap = XST_RATE * 2;
    unsigned rate, ch;
    size_t n, i;
    int same = 1;

    /* 44.1 kHz stereo: the samples as they are. */
    write_wav(in_work("d441.wav"), 44100, 2, 1000, 500);
    n = decode_all(in_work("d441.wav"), pcm, cap, &rate, &ch);
    CHECK("44.1 kHz length", n == 22050 && rate == 44100 && ch == 2);
    for (i = 0; i < n; i++) {
        int16_t want = (int16_t)lrint(16384.0 * sin(2 * 3.14159265358979323846 * 1000 * i / 44100.0));
        if (abs(pcm[i * 2] - want) > 1 || pcm[i * 2] != pcm[i * 2 + 1])
            same = 0;
    }
    CHECK("44.1 kHz passes through", same);

    /* 48 kHz mono: resampled, both channels, the tone and its level kept. */
    write_wav(in_work("d48.wav"), 48000, 1, 1000, 1000);
    n = decode_all(in_work("d48.wav"), pcm, cap, &rate, &ch);
    CHECK("48 kHz length", n >= 44098 && n <= 44102 && rate == 48000 && ch == 1);
    CHECK("48 kHz: 1 kHz left", tone_share(pcm + 2000, n - 4000, 0, 1000) > 0.999);
    CHECK("48 kHz: 1 kHz right", tone_share(pcm + 2000, n - 4000, 1, 1000) > 0.999);
    CHECK("48 kHz level", fabs(rms_db(pcm + 2000, n - 4000) - (-9.03)) < 0.05);

    /* 22.05 kHz (upsampling) and 96 kHz (more than 2:1 down). */
    write_wav(in_work("d22.wav"), 22050, 2, 1000, 1000);
    n = decode_all(in_work("d22.wav"), pcm, cap, &rate, &ch);
    CHECK("22.05 kHz", n >= 44098 && n <= 44102 && tone_share(pcm + 2000, n - 4000, 0, 1000) > 0.999
                       && fabs(rms_db(pcm + 2000, n - 4000) - (-9.03)) < 0.05);
    write_wav(in_work("d96.wav"), 96000, 2, 1000, 1000);
    n = decode_all(in_work("d96.wav"), pcm, cap, &rate, &ch);
    CHECK("96 kHz", n >= 44098 && n <= 44102 && tone_share(pcm + 2000, n - 4000, 0, 1000) > 0.999
                    && fabs(rms_db(pcm + 2000, n - 4000) - (-9.03)) < 0.05);
    /* A tone above the output's Nyquist rate must not fold back. */
    write_wav(in_work("d96hi.wav"), 96000, 2, 30000, 1000);
    n = decode_all(in_work("d96hi.wav"), pcm, cap, &rate, &ch);
    CHECK("30 kHz at 96 kHz is filtered out", n > 44000 && rms_db(pcm + 2000, n - 4000) < -70.0);

    /* The encoded formats: the 1 kHz tone comes out, at its level. */
    fixture("tone_id3v23.mp3", "f.mp3");
    n = decode_all(in_work("f.mp3"), pcm, cap, &rate, &ch);
    /* LAME at 64 kbit/s scales its input down a little: -9.5 dB, not -9.0. */
    CHECK("mp3", n > 21000 && n < 25000 && rate == 44100
                 && tone_share(pcm + 4000, n - 8000, 0, 1000) > 0.99
                 && fabs(rms_db(pcm + 4000, n - 8000) - (-9.03)) < 0.6);
    fixture("tone48_id3v24.mp3", "f48.mp3");
    n = decode_all(in_work("f48.mp3"), pcm, cap, &rate, &ch);
    CHECK("mp3 48 kHz", n > 21000 && n < 25000 && rate == 48000
                        && tone_share(pcm + 4000, n - 8000, 1, 1000) > 0.99);
    fixture("tone.flac", "f.flac");
    n = decode_all(in_work("f.flac"), pcm, cap, &rate, &ch);
    CHECK("flac", n == 22050 && tone_share(pcm, n, 0, 1000) > 0.999
                  && fabs(rms_db(pcm, n) - (-9.03)) < 0.05);
    fixture("tone48.ogg", "f48.ogg");
    n = decode_all(in_work("f48.ogg"), pcm, cap, &rate, &ch);
    CHECK("ogg", n > 21900 && n < 22200 && rate == 48000
                 && tone_share(pcm + 4000, n - 8000, 0, 1000) > 0.99
                 && fabs(rms_db(pcm + 4000, n - 8000) - (-9.03)) < 0.3);

#if defined(__APPLE__)
    /* macOS: m4a through the system's decoder, AAC and Apple Lossless. */
    fixture("tone_aac.m4a", "f_aac.m4a");
    n = decode_all(in_work("f_aac.m4a"), pcm, cap, &rate, &ch);
    CHECK("m4a aac", n > 21000 && n < 26000 && rate == 44100 && ch == 2
                     && tone_share(pcm + 4000, n - 8000, 0, 1000) > 0.99
                     && fabs(rms_db(pcm + 4000, n - 8000) - (-9.03)) < 0.3);
    fixture("tone_alac.m4a", "f_alac.m4a");
    n = decode_all(in_work("f_alac.m4a"), pcm, cap, &rate, &ch);
    CHECK("m4a apple lossless", n == 22050 && tone_share(pcm, n, 0, 1000) > 0.999
                                && fabs(rms_db(pcm, n) - (-9.03)) < 0.05);
    {
        uint32_t ms = 0;
        char title[8];
        CHECK("probe m4a", xst_probe(in_work("f_alac.m4a"), &ms, title, sizeof title) && ms == 500);
    }
#endif

    {
        char title[64];
        uint32_t ms = 0;
        CHECK("probe mp3 v2.4", xst_probe(in_work("f48.mp3"), &ms, title, sizeof title)
                                && !strcmp(title, "Tone 48k") && ms > 400 && ms < 650);
        CHECK("probe a short title buffer", xst_probe(in_work("f.mp3"), &ms, title, 4)
                                            && strlen(title) <= 3);
        CHECK("probe a text file", !xst_probe(in_work("lib/notes.txt"), &ms, title, sizeof title));
        CHECK("open a text file", xst_stream_open(in_work("lib/notes.txt")) == NULL);
        CHECK("open a missing file", xst_stream_open(in_work("none.mp3")) == NULL);
        CHECK("open a broken flac", xst_stream_open(in_work("lib/broken.flac")) == NULL);
    }
}

static void wait_counts(void)
{
    int tries, i, pending = 1;
    for (tries = 0; tries < 400 && pending; tries++) {
        xsb_row r;
        pending = 0;
        for (i = 0; xsb_row_at(i, &r); i++)
            if (r.songs < 0)
                pending = 1;
        if (pending) {
#if defined(_WIN32)
            Sleep(10);
#else
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
#endif
        }
    }
    CHECK("counts arrive", !pending);
}

static int find_row(const char *label)
{
    xsb_row r;
    int i;
    for (i = 0; xsb_row_at(i, &r); i++)
        if (!strcmp(r.label, label))
            return i;
    return -1;
}

/* The browser's model over the tree test_scan_and_db made (lib/) and a made
 * home folder and media root. */
static void test_browse(void)
{
    char home[1400], media[1400], chosen[1024], where[1024];
    const char *roots[2];
    xsb_row r;
    int i, sel = -1;

    snprintf(home, sizeof home, "%s", in_work("home/deck"));
    snprintf(media, sizeof media, "%s", in_work("media"));
    mkdir(in_work("home"), 0755);
    mkdir(home, 0755);
    mkdir(in_work("home/deck/Music"), 0755);
    mkdir(in_work("home/deck/Music/Album"), 0755);
    mkdir(in_work("home/deck/Music/Empty"), 0755);
    mkdir(in_work("home/deck/Music/.hidden"), 0755);
    mkdir(media, 0755);
    mkdir(in_work("media/deck"), 0755);
    mkdir(in_work("media/deck/SDCARD"), 0755);
    mkdir(in_work("media/USB"), 0755);
    /* A Downloads that cannot be listed (no permission, as Android's shared
     * folders before the audio permission): left out like a missing one. */
#if !defined(_WIN32)
    mkdir(in_work("home/deck/Downloads"), 0000);
#endif
    write_wav(in_work("home/deck/Music/loose.wav"), 8000, 1, 440, 10);
    write_wav(in_work("home/deck/Music/Album/1.wav"), 8000, 1, 440, 10);
    write_wav(in_work("home/deck/Music/Album/2.wav"), 8000, 1, 440, 10);
    copy_file(in_work("home/deck/Music/loose.wav"), in_work("home/deck/Music/Album/cover.jpg"));
    roots[0] = media; roots[1] = NULL;
    xsb_set_roots(home, roots);

    CHECK("closed: no rows", xsb_rows() == 0 && !xsb_is_open());
    xsb_open(in_work("lib"));
    CHECK("open", xsb_is_open());
    /* The places: off, the current folder, Music, Home, the card under the
     * user's name and the drive beside it; no Downloads (there is none). */
    CHECK("turn off first", xsb_row_at(0, &r) && r.kind == XSB_OFF);
    CHECK("current folder", xsb_row_at(1, &r) && r.kind == XSB_PLACE
                            && !strcmp(r.label, "Current folder: lib"));
    CHECK("the folder it is in", xsb_row_at(2, &r) && r.kind == XSB_PLACE
                                 && !strncmp(r.label, "The folder it is in: ", 21));
    CHECK("music", find_row("Music") == 3);
    CHECK("home", find_row("Home") == 4);
    CHECK("no downloads", find_row("Downloads") < 0);
    CHECK("the user's card", find_row("Drive: SDCARD") == 5);
    CHECK("a drive", find_row("Drive: USB") == 6);
    CHECK("not the user's folder itself", find_row("Drive: deck") < 0);
    CHECK("seven places", xsb_rows() == 7);
    xsb_where(where, sizeof where);
    CHECK("on the places", where[0] == 0);
    wait_counts();
    /* By name: the broken .flac counts here, a scan leaves it out. */
    CHECK("songs under the current folder", xsb_row_at(1, &r) && r.songs == 22 && !r.more);
    CHECK("songs under Music", xsb_row_at(3, &r) && r.songs == 3);
    CHECK("an empty card", xsb_row_at(5, &r) && r.songs == 0);

    CHECK("into Music", xsb_activate(3, chosen, sizeof chosen, &sel) == XSB_MOVED);
    xsb_where(where, sizeof where);
    CHECK("where", !strcmp(where, in_work("home/deck/Music")));
    CHECK("use, up, then the folders", xsb_rows() == 4 && xsb_row_at(0, &r) && r.kind == XSB_USE
                                       && xsb_row_at(1, &r) && r.kind == XSB_UP
                                       && xsb_row_at(2, &r) && r.kind == XSB_DIR && !strcmp(r.label, "Album")
                                       && xsb_row_at(3, &r) && !strcmp(r.label, "Empty"));
    CHECK("cursor on the first folder", sel == 2);
    wait_counts();
    CHECK("counts", xsb_row_at(0, &r) && r.songs == 3 && xsb_row_at(2, &r) && r.songs == 2
                    && xsb_row_at(3, &r) && r.songs == 0);
    CHECK("into Album", xsb_activate(2, chosen, sizeof chosen, &sel) == XSB_MOVED && sel == 0);
    CHECK("no folders there", xsb_rows() == 2);
    CHECK("use it", xsb_activate(0, chosen, sizeof chosen, &sel) == XSB_CHOSEN
                    && !strcmp(chosen, in_work("home/deck/Music/Album")));
    /* Up: the folder left is under the cursor. */
    CHECK("..", xsb_activate(1, chosen, sizeof chosen, &sel) == XSB_MOVED && sel == 2);
    xsb_where(where, sizeof where);
    CHECK("in Music again", !strcmp(where, in_work("home/deck/Music")));
    /* Up from the place's own folder: the places, on that place. */
    CHECK("back to the places", xsb_back(&sel) == XSB_MOVED && sel == 3 && xsb_rows() == 7);
    CHECK("back from the places closes", xsb_back(&sel) == XSB_CLOSED);
    CHECK("turn off", xsb_activate(0, chosen, sizeof chosen, &sel) == XSB_TURNED_OFF);
    /* Home: its Music is a folder of it; going up from it stays inside Home. */
    CHECK("into Home", xsb_activate(4, chosen, sizeof chosen, &sel) == XSB_MOVED);
    i = find_row("Music");
    CHECK("Music in Home", i == 2 && xsb_activate(i, chosen, sizeof chosen, &sel) == XSB_MOVED);
    CHECK("up to Home", xsb_back(&sel) == XSB_MOVED && sel == 2);
    xsb_where(where, sizeof where);
    CHECK("Home", !strcmp(where, home));
    xsb_close();
    CHECK("closed", !xsb_is_open() && xsb_rows() == 0);

    /* BO-243: the folder in use is a soundtrack of a folder of them. The
     * one it is in is the row under it; in the folder in use the cursor is
     * on "..", which goes to that folder (not back to the places), and up
     * on from there. */
    xsb_open(in_work("home/deck/Music/Album"));
    CHECK("in use: current", xsb_row_at(1, &r) && !strcmp(r.label, "Current folder: Album"));
    CHECK("in use: the folder it is in", xsb_row_at(2, &r) && !strcmp(r.label, "The folder it is in: Music")
                                         && !strcmp(r.path, in_work("home/deck/Music")));
    CHECK("in use: into it", xsb_activate(1, chosen, sizeof chosen, &sel) == XSB_MOVED && sel == 1);
    xsb_row_at(sel, &r);
    CHECK("in use: on ..", r.kind == XSB_UP);
    CHECK("in use: .. to the folder it is in", xsb_activate(sel, chosen, sizeof chosen, &sel) == XSB_MOVED);
    xsb_where(where, sizeof where);
    CHECK("in use: in Music", !strcmp(where, in_work("home/deck/Music")));
    CHECK("in use: on Album", xsb_row_at(sel, &r) && !strcmp(r.label, "Album"));
    CHECK("in use: choose Music", xsb_activate(0, chosen, sizeof chosen, &sel) == XSB_CHOSEN
                                  && !strcmp(chosen, in_work("home/deck/Music")));
    CHECK("in use: up on from it", xsb_back(&sel) == XSB_MOVED);
    xsb_where(where, sizeof where);
    CHECK("in use: in home/deck", !strcmp(where, in_work("home/deck")));
    xsb_close();
    /* The row under it, entered from the places: one step to the parent. */
    xsb_open(in_work("home/deck/Music/Album"));
    CHECK("parent row: into it", xsb_activate(2, chosen, sizeof chosen, &sel) == XSB_MOVED);
    xsb_where(where, sizeof where);
    CHECK("parent row: Music", !strcmp(where, in_work("home/deck/Music")));
    CHECK("parent row: up on", xsb_back(&sel) == XSB_MOVED);
    xsb_where(where, sizeof where);
    CHECK("parent row: home/deck", !strcmp(where, in_work("home/deck")));
    xsb_close();

    /* The chosen folder is gone (a card taken out, deleted): no dead
     * "Current folder" row, "turn off" stays, the places as before. */
    xsb_open(in_work("gone/Album"));
    CHECK("gone: turn off first", xsb_row_at(0, &r) && r.kind == XSB_OFF);
    CHECK("gone: no current folder row", find_row("Current folder: Album") < 0
                                          && xsb_row_at(1, &r) && !strcmp(r.label, "Music"));
    CHECK("gone: the places", xsb_rows() == 5);
    xsb_close();

    /* Nothing chosen yet: no "turn off", no current folder. */
    xsb_open("");
    CHECK("no current folder", xsb_rows() == 4 && xsb_row_at(0, &r) && !strcmp(r.label, "Music"));
    xsb_close();
    xsb_set_roots(NULL, NULL);
#if !defined(_WIN32)
    chmod(in_work("home/deck/Downloads"), 0755);
#endif
}

/* A folder and files with letters outside ASCII and outside Latin-1, as
 * UTF-8: found by the scan, listed by the browser, opened by every decoder.
 * (On Windows this is the wide-character road end to end.) */
static void test_utf8_paths(void)
{
    /* "Árvíztűrő tükörfúrógép", "Őszi dal", and a decomposed "ő". */
    static const char dir[] = "u8/\xC3\x81rv\xC3\xADzt\xC5\xB1r\xC5\x91 t\xC3\xBCk\xC3\xB6rf\xC3\xBAr\xC3\xB3g\xC3\xA9p";
    static const char sub[] = "\xC5\x90szi dal";
    char path[512], chosen[1024], where[1024], label[64];
    static int16_t pcm[XST_RATE];
    xst_library *lib;
    xst_stream *st;
    xsb_row r;
    uint32_t ms = 0;
    int sel, i;

    mkdir(in_work("u8"), 0755);
    mkdir(in_work(dir), 0755);
    snprintf(path, sizeof path, "%s/%s", dir, sub);
    mkdir(in_work(path), 0755);
    snprintf(path, sizeof path, "%s/%s/1 \xC5\xB1r.wav", dir, sub);
    write_wav(in_work(path), 44100, 2, 1000, 200);
    snprintf(path, sizeof path, "%s/%s/2 \xC5\x91.mp3", dir, sub);
    fixture("tone_id3v23.mp3", path);
    snprintf(path, sizeof path, "%s/%s/3 \xC5\x91.flac", dir, sub);
    fixture("tone.flac", path);
    snprintf(path, sizeof path, "%s/%s/4 \xC5\x91.ogg", dir, sub);
    fixture("tone48.ogg", path);

    lib = xst_scan(in_work(dir));
    CHECK("scan a folder named in UTF-8", lib && lib->n_st == 1 && lib->n_songs == 4);
    if (lib) {
        /* The title's menu name: folded to Latin-1 (O with diaeresis). */
        CHECK("soundtrack name", lib->st[0].name[0] == 0xD6 && lib->st[0].name[1] == 's');
        CHECK("wav name and length", lib->songs[0].name[2] == 0xFC && lib->songs[0].ms == 200);
        CHECK("the path is kept as it is", strstr(lib->songs[0].path, "\xC5\x90szi dal/1 \xC5\xB1r.wav") != NULL);
        for (i = 0; i < lib->n_songs; i++) {
            st = xst_stream_open(lib->songs[i].path);
            CHECK("open a song by its UTF-8 path", st != NULL && xst_stream_read(st, pcm, 1000) == 1000);
            xst_stream_close(st);
            CHECK("probe it", xst_probe(lib->songs[i].path, &ms, NULL, 0) && ms >= 190);
        }
    }
    xst_free(lib);

    xsb_set_roots(in_work("u8"), NULL);
    xsb_open(in_work(dir));
    CHECK("the current folder's label", xsb_row_at(1, &r) && strstr(r.label, "t\xC5\xB1r\xC5\x91") != NULL);
    CHECK("into it", xsb_activate(1, chosen, sizeof chosen, &sel) == XSB_MOVED && xsb_rows() == 3);
    CHECK("its folder", xsb_row_at(2, &r) && !strcmp(r.label, sub));
    wait_counts();
    CHECK("its songs", xsb_row_at(2, &r) && r.songs == 4);
    CHECK("into that", xsb_activate(2, chosen, sizeof chosen, &sel) == XSB_MOVED);
    CHECK("choose", xsb_activate(0, chosen, sizeof chosen, &sel) == XSB_CHOSEN);
    xsb_where(where, sizeof where);
    CHECK("the chosen path is the UTF-8 one", !strcmp(chosen, where)
                                             && strstr(chosen, "\xC5\x90szi dal") != NULL);
    lib = xst_scan(chosen);
    CHECK("and scans", lib && lib->n_songs == 4);
    xst_free(lib);
    xsb_close();
    xsb_set_roots(NULL, NULL);

    /* What a screen is given: a decomposed name composed. */
    xst_utf8_compose(label, sizeof label, "O\xCC\x8Bszi u\xCC\x8Brhajo\xCC\x81 c\xCC\x8C z\xCC\x87 x\xCC\x81");
    CHECK("composed", !strcmp(label, "\xC5\x90szi \xC5\xB1rhaj\xC3\xB3 \xC4\x8D \xC5\xBC x\xCC\x81"));
    xst_utf8_compose(label, sizeof label, "\xD0\x96 \xCE\xA9 \xE6\x97\xA5 abc");
    CHECK("other scripts untouched", !strcmp(label, "\xD0\x96 \xCE\xA9 \xE6\x97\xA5 abc"));
    xst_utf8_compose(label, 6, "ab\xC5\x91\xC5\x91\xC5\x91");
    CHECK("cut on a character", !strcmp(label, "ab\xC5\x91"));
    xst_utf8_compose(label, sizeof label, "a\xFFz");
    CHECK("a broken byte", !strcmp(label, "a\xEF\xBF\xBDz"));
}

/* An mp3 as one was found in a real folder: 64 KiB of zeros, the tail of a
 * frame cut in two, then the frames; no tag. A decoder plays it; the scan's
 * probe has to take it too. And what is left out is named, with the reason. */
static void test_mp3_junk_and_left_out(void)
{
    static uint8_t mp3[8192];
    static int16_t pcm[XST_RATE * 2];
    FILE *f;
    size_t n, i, start = 0;
    uint32_t ms = 0;
    unsigned rate, ch;
    const char *why = NULL;
    xst_library *lib;
    char from[1400];
    int k, found_zero = 0, found_text = 0, found_mp3 = 0;

    snprintf(from, sizeof from, "%s/tone_id3v23.mp3", g_data);
    f = u8_fopen(from, "rb");
    n = f ? fread(mp3, 1, sizeof mp3, f) : 0;
    if (f) fclose(f);
    CHECK("fixture", n > 2000 && !memcmp(mp3, "ID3", 3));
    if (n > 10)
        start = 10 + (((size_t)mp3[6] & 0x7F) << 21 | ((size_t)mp3[7] & 0x7F) << 14
                      | ((size_t)mp3[8] & 0x7F) << 7 | ((size_t)mp3[9] & 0x7F));
    CHECK("frames after the tag", start < n && mp3[start] == 0xFF);

    mkdir(in_work("junk"), 0755);
    f = u8_fopen(in_work("junk/1 zeros then frames.mp3"), "wb");
    for (i = 0; i < 65536; i++) fputc(0, f);
    for (i = 0; i < 168; i++) fputc((int)(0xE2 + i * 37) & 0xFF, f);     /* no frame header in it */
    fwrite(mp3 + start, 1, n - start, f);
    fclose(f);
    f = u8_fopen(in_work("junk/2 only zeros.mp3"), "wb");
    for (i = 0; i < 70000; i++) fputc(0, f);
    fclose(f);
    f = u8_fopen(in_work("junk/3 text.mp3"), "wb");
    /* 0xFF 0xFB once, by chance, with no frame after it. */
    fputs("not music \xFF\xFB\x92\x44 at all, and long enough to hold the frame that is not there", f);
    for (i = 0; i < 600; i++) fputc('x', f);
    fclose(f);
    f = u8_fopen(in_work("junk/4 empty.mp3"), "wb");
    fclose(f);
    copy_file(in_work("junk/3 text.mp3"), in_work("junk/5 text.flac"));

    CHECK("zeros, junk, then frames: taken", xst_probe_why(in_work("junk/1 zeros then frames.mp3"), &ms,
                                                           NULL, 0, &why) && ms > 300 && ms < 900);
    n = decode_all(in_work("junk/1 zeros then frames.mp3"), pcm, XST_RATE, &rate, &ch);
    CHECK("and it plays", n > 20000 && tone_share(pcm + 8000, n - 12000, 0, 1000) > 0.99);
    CHECK("only zeros: not", !xst_probe_why(in_work("junk/2 only zeros.mp3"), &ms, NULL, 0, &why)
                             && strstr(why, "no MPEG audio"));
    CHECK("a header by chance: not", !xst_probe_why(in_work("junk/3 text.mp3"), &ms, NULL, 0, &why)
                                     && strstr(why, "no MPEG audio"));
    CHECK("empty: not", !xst_probe_why(in_work("junk/4 empty.mp3"), &ms, NULL, 0, &why)
                        && strstr(why, "empty"));
    CHECK("no such file", !xst_probe_why(in_work("junk/none.mp3"), &ms, NULL, 0, &why)
                          && strstr(why, "cannot be opened"));
    CHECK("not a flac", !xst_probe_why(in_work("junk/5 text.flac"), &ms, NULL, 0, &why)
                        && strstr(why, "FLAC"));

    lib = xst_scan(in_work("junk"));
    CHECK("one song, four left out", lib && lib->n_songs == 1 && lib->bad_files == 4
                                     && lib->n_left_out == 4);
    for (k = 0; lib && k < lib->n_left_out; k++) {
        if (strstr(lib->left_out[k].path, "2 only zeros.mp3") && strstr(lib->left_out[k].why, "MPEG"))
            found_zero = 1;
        if (strstr(lib->left_out[k].path, "5 text.flac") && strstr(lib->left_out[k].why, "FLAC"))
            found_text = 1;
        if (strstr(lib->left_out[k].path, "4 empty.mp3") && strstr(lib->left_out[k].why, "empty"))
            found_mp3 = 1;
    }
    CHECK("each named with its reason", found_zero && found_text && found_mp3);
    xst_free(lib);
}

static void test_progress(void)
{
    xst_progress pr;
    xst_library *lib;

    memset(&pr, 0, sizeof pr);
    lib = xst_scan_progress(in_work("lib"), &pr);
    CHECK("progress counts", lib && pr.songs == lib->n_songs && pr.files == lib->n_songs + 1
                             && pr.folders >= 4);
    xst_free(lib);
    memset(&pr, 0, sizeof pr);
    pr.cancel = 1;
    CHECK("a cancelled scan", xst_scan_progress(in_work("lib"), &pr) == NULL && pr.files == 0);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: soundtrack_test <work dir> <fixture dir>\n");
        return 2;
    }
    snprintf(g_work, sizeof g_work, "%s", argv[1]);
    snprintf(g_data, sizeof g_data, "%s", argv[2]);
    mkdir(g_work, 0755);
    test_names();
    test_scan_and_db();
    test_limits();
    test_browse();
    test_utf8_paths();
    test_mp3_junk_and_left_out();
    test_progress();
    test_decode();
    printf(failures ? "%d FAILED\n" : "ok\n", failures);
    return failures ? 1 : 0;
}

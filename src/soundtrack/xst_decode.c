/*
 * xst_decode.c - a music file as 44.1 kHz 16-bit stereo, and what a scan
 * needs of it (length, title) without decoding. See xbox_soundtrack.h.
 *
 * Decoders, each one file, vendored unmodified under third_party/:
 *   dr_mp3, dr_flac, dr_wav   public domain or MIT-0 (David Reid)
 *   stb_vorbis                public domain or MIT (Sean Barrett)
 */
#include "xbox_soundtrack.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-value"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wcomma"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define DR_MP3_IMPLEMENTATION
#define DRMP3_API static
#define DRMP3_PRIVATE static
#include "dr_libs/dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#define DRFLAC_API static
#define DRFLAC_PRIVATE static
#include "dr_libs/dr_flac.h"
#define DR_WAV_IMPLEMENTATION
#define DRWAV_API static
#define DRWAV_PRIVATE static
#include "dr_libs/dr_wav.h"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#include "stb_vorbis/stb_vorbis.c"
/* stb_vorbis.c leaves its one-letter helpers defined. */
#undef L
#undef C
#undef R
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ── files ─────────────────────────────────────────────────────────── */

typedef enum { K_NONE, K_MP3, K_FLAC, K_OGG, K_WAV, K_SYSTEM } xst_kind;

static xst_kind kind_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    char e[8];
    int i;

    if (!dot || strlen(dot) >= sizeof e)
        return K_NONE;
    for (i = 0; dot[i]; i++)
        e[i] = (char)tolower((unsigned char)dot[i]);
    e[i] = 0;
    if (!strcmp(e, ".mp3"))  return K_MP3;
    if (!strcmp(e, ".flac")) return K_FLAC;
    if (!strcmp(e, ".ogg") || !strcmp(e, ".oga")) return K_OGG;
    if (!strcmp(e, ".wav"))  return K_WAV;
#if defined(__APPLE__)
    /* What the system decodes (sys_* below): Apple Music's and iTunes' own. */
    if (!strcmp(e, ".m4a") || !strcmp(e, ".aac") || !strcmp(e, ".aif") || !strcmp(e, ".aiff")
        || !strcmp(e, ".caf"))
        return K_SYSTEM;
#endif
    return K_NONE;
}

#if defined(_WIN32)
/* Paths are UTF-8 here; the C library's narrow calls are not. */
static int wide_path(const char *path, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, path, -1, out, cap) > 0;
}
#endif

static FILE *open_file(const char *path)
{
#if defined(_WIN32)
    wchar_t w[1024];
    return wide_path(path, w, 1024) ? _wfopen(w, L"rb") : NULL;
#else
    return fopen(path, "rb");
#endif
}

static void copy_text(char *out, size_t cap, const char *s, size_t n)
{
    if (!cap)
        return;
    while (n && (*s == ' ' || *s == '\t')) { s++; n--; }
    if (n >= cap) {
        n = cap - 1;
        /* Not through the middle of a UTF-8 character. */
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, s, n);
    out[n] = 0;
}

/* ── mp3: ID3v2 title and a length from the first frame ────────────── */

static size_t utf8_put(char *o, size_t n, size_t cap, uint32_t c)
{
    if (c < 0x80) {
        if (n + 1 < cap) o[n++] = (char)c;
    } else if (c < 0x800) {
        if (n + 2 < cap) { o[n++] = (char)(0xC0 | (c >> 6)); o[n++] = (char)(0x80 | (c & 0x3F)); }
    } else {
        if (c > 0xFFFF) c = '?';
        if (c == '?') { if (n + 1 < cap) o[n++] = '?'; }
        else if (n + 3 < cap) {
            o[n++] = (char)(0xE0 | (c >> 12));
            o[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            o[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    return n;
}

/* An ID3v2 text frame's body (encoding byte first) as UTF-8. */
static void id3_text(const uint8_t *b, size_t len, char *out, size_t cap)
{
    size_t n = 0, i;
    int enc;

    if (!cap)
        return;
    out[0] = 0;
    if (len < 2)
        return;
    enc = b[0]; b++; len--;
    if (enc == 0) {                               /* ISO-8859-1 */
        for (i = 0; i < len && b[i]; i++)
            n = utf8_put(out, n, cap, b[i]);
    } else if (enc == 3) {                        /* UTF-8 */
        for (i = 0; i < len && b[i]; i++)
            ;
        copy_text(out, cap, (const char *)b, i);
        return;
    } else {                                      /* UTF-16, BOM or BE */
        int be = enc == 2;
        i = 0;
        if (enc == 1 && len >= 2) {
            if (b[0] == 0xFE && b[1] == 0xFF) { be = 1; i = 2; }
            else if (b[0] == 0xFF && b[1] == 0xFE) { be = 0; i = 2; }
        }
        for (; i + 1 < len; i += 2) {
            uint32_t c = be ? (uint32_t)(b[i] << 8 | b[i + 1]) : (uint32_t)(b[i + 1] << 8 | b[i]);
            if (!c)
                break;
            if (c >= 0xD800 && c <= 0xDFFF)
                c = '?';
            n = utf8_put(out, n, cap, c);
        }
    }
    out[n] = 0;
}

static uint32_t syncsafe(const uint8_t *p)
{
    return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14
         | (uint32_t)(p[2] & 0x7F) << 7 | (uint32_t)(p[3] & 0x7F);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The tag's title into `title`; returns the tag's whole size (0: no tag). */
static long id3v2_title(FILE *f, char *title, size_t cap)
{
    uint8_t h[10], *tag;
    uint32_t size, pos = 0;
    long full;
    int ver;

    if (fseek(f, 0, SEEK_SET) != 0 || fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3))
        return 0;
    ver = h[3];
    size = syncsafe(h + 6);
    full = (long)size + 10 + ((h[5] & 0x10) ? 10 : 0);   /* footer */
    if (ver < 2 || ver > 4 || size == 0)
        return full;
    /* The text frames come first; a cover picture after them is megabytes
     * a scan of a whole library should not read. */
    if (size > 65536)
        size = 65536;
    tag = (uint8_t *)malloc(size);
    if (!tag)
        return full;
    if (fread(tag, 1, size, f) != size) {
        free(tag);
        return full;
    }
    if (ver >= 3 && (h[5] & 0x40) && size >= 4)   /* extended header */
        pos = ver == 4 ? syncsafe(tag) : be32(tag) + 4;
    while (!(h[5] & 0x80)) {                      /* not unsynchronised */
        uint32_t fsize, hdr = ver == 2 ? 6 : 10;
        int is_title;
        /* Not pos + hdr > size: a corrupt extended-header size makes pos
         * huge and that sum wraps (audit finding 16). */
        if (pos > size || hdr > size - pos || tag[pos] == 0)
            break;
        if (ver == 2) {
            fsize = (uint32_t)tag[pos + 3] << 16 | (uint32_t)tag[pos + 4] << 8 | tag[pos + 5];
            is_title = !memcmp(tag + pos, "TT2", 3);
        } else {
            fsize = ver == 4 ? syncsafe(tag + pos + 4) : be32(tag + pos + 4);
            is_title = !memcmp(tag + pos, "TIT2", 4);
        }
        if (fsize > size - pos - hdr)
            break;
        if (is_title) {
            id3_text(tag + pos + hdr, fsize, title, cap);
            break;
        }
        pos += hdr + fsize;
    }
    free(tag);
    return full;
}

/* A layer III frame header at p: its length in bytes (0: not one), and
 * what a length needs. Free-format frames (bitrate index 0) have no length
 * in the header and are not looked for. */
static unsigned mp3_frame(const uint8_t *p, unsigned *kbps, unsigned *rate, unsigned *spf, int *mono,
                          int *mpeg1)
{
    static const uint16_t br1[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
    static const uint16_t br2[16] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
    static const uint32_t sr[3] = { 44100, 48000, 32000 };
    unsigned ver, layer, bri, sri, pad;

    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0)
        return 0;
    ver = (p[1] >> 3) & 3;                  /* 3: MPEG1, 2: MPEG2, 0: 2.5 */
    layer = (p[1] >> 1) & 3;                /* 1: layer III */
    bri = p[2] >> 4;
    sri = (p[2] >> 2) & 3;
    pad = (p[2] >> 1) & 1;
    if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3)
        return 0;
    *mpeg1 = ver == 3;
    *kbps = ver == 3 ? br1[bri] : br2[bri];
    *rate = sr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
    *spf = ver == 3 ? 1152 : 576;
    *mono = (p[3] >> 6) == 3;
    return (ver == 3 ? 144u : 72u) * *kbps * 1000u / *rate + pad;
}

/* How far past the tag the first frame is looked for. A decoder
 * resynchronises through anything; files do start with junk: a block of
 * zeros a ripper or a download manager left (64 KiB of them in one seen),
 * the tail of a frame cut in two, a second tag. */
#define MP3_SEARCH_NEAR  16384
#define MP3_SEARCH_FAR   (1 << 20)

static int mp3_probe(const char *path, uint32_t *ms, char *title, size_t cap, const char **why)
{
    FILE *f = open_file(path);
    uint8_t first[MP3_SEARCH_NEAR], *more = NULL, *buf = first;
    long start, end;
    size_t got, i;
    int ok = 0, pass;

    if (!f) {
        *why = "cannot be opened";
        return 0;
    }
    start = id3v2_title(f, title, cap);
    fseek(f, 0, SEEK_END);
    end = ftell(f);
    if (end <= 0) {
        fclose(f);
        *why = "an empty file";
        return 0;
    }
    /* The first 16 KiB is where the first frame nearly always is; only a
     * file with none there is read further. */
    for (pass = 0; pass < 2 && !ok; pass++) {
        size_t want = pass ? MP3_SEARCH_FAR : MP3_SEARCH_NEAR;
        if (pass) {
            more = (uint8_t *)malloc(MP3_SEARCH_FAR);
            if (!more)
                break;
            buf = more;
        }
        if (fseek(f, start, SEEK_SET) != 0)
            break;
        got = fread(buf, 1, want, f);
        for (i = 0; i + 4 <= got; i++) {
            unsigned kbps, rate, spf, len, k2, r2, s2, off;
            int mono, mpeg1, m2, v2;
            if (buf[i] != 0xFF)
                continue;
            len = mp3_frame(buf + i, &kbps, &rate, &spf, &mono, &mpeg1);
            if (!len)
                continue;
            /* Two bytes of a header turn up by chance in junk: the next
             * frame must be where this one says, when that is in sight. */
            if (i + len + 4 <= got && !mp3_frame(buf + i + len, &k2, &r2, &s2, &m2, &v2))
                continue;
            ok = 1;
            *ms = 0;
            off = (unsigned)i + 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
            if (off + 12 < got && (!memcmp(buf + off, "Xing", 4) || !memcmp(buf + off, "Info", 4))
                && (buf[off + 7] & 1)) {
                uint32_t frames = be32(buf + off + 8);
                *ms = (uint32_t)((uint64_t)frames * spf * 1000 / rate);
            } else if (kbps && end > start + (long)i) {
                *ms = (uint32_t)((uint64_t)(end - start - (long)i) * 8 / kbps);
            }
            break;
        }
        if (got < want)
            break;                          /* the whole file was looked at */
    }
    free(more);
    fclose(f);
    if (!ok)
        *why = "no MPEG audio found in its first megabyte";
    return ok;
}

/* ── macOS: the system's decoder for m4a (AAC, Apple Lossless) ─────── */

/*
 * No AAC decoder with a licence this project can ship is small enough to
 * vendor (FAAD2 is GPL, FDK-AAC's licence is its own, FFmpeg's is LGPL and
 * large). On macOS the system has one: AudioToolbox's ExtAudioFile reads
 * m4a (AAC and Apple Lossless), AIFF and CAF, so an Apple Music or iTunes
 * library plays there. Protected files (m4p) do not open. Other hosts do
 * not list these files.
 */
#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>

typedef struct {
    ExtAudioFileRef file;
    unsigned rate, channels;
    uint64_t frames;
} sys_audio;

static CFURLRef sys_url(const char *path)
{
    return CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                   (CFIndex)strlen(path), false);
}

/* Open for reading as 32-bit float at the file's own rate and channels. */
static int sys_open(sys_audio *a, const char *path)
{
    AudioStreamBasicDescription in, out;
    UInt32 size = sizeof in;
    SInt64 frames = 0;
    CFURLRef url = sys_url(path);
    OSStatus st;

    memset(a, 0, sizeof *a);
    if (!url)
        return 0;
    st = ExtAudioFileOpenURL(url, &a->file);
    CFRelease(url);
    if (st != noErr || !a->file)
        return 0;
    memset(&in, 0, sizeof in);
    if (ExtAudioFileGetProperty(a->file, kExtAudioFileProperty_FileDataFormat, &size, &in) != noErr
        || in.mSampleRate < 4000 || in.mChannelsPerFrame < 1 || in.mChannelsPerFrame > 8)
        goto fail;
    memset(&out, 0, sizeof out);
    out.mSampleRate = in.mSampleRate;
    out.mFormatID = kAudioFormatLinearPCM;
    out.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    out.mBitsPerChannel = 32;
    out.mChannelsPerFrame = in.mChannelsPerFrame;
    out.mBytesPerFrame = 4 * out.mChannelsPerFrame;
    out.mFramesPerPacket = 1;
    out.mBytesPerPacket = out.mBytesPerFrame;
    if (ExtAudioFileSetProperty(a->file, kExtAudioFileProperty_ClientDataFormat, sizeof out, &out) != noErr)
        goto fail;
    size = sizeof frames;
    if (ExtAudioFileGetProperty(a->file, kExtAudioFileProperty_FileLengthFrames, &size, &frames) == noErr
        && frames > 0)
        a->frames = (uint64_t)frames;
    a->rate = (unsigned)in.mSampleRate;
    a->channels = in.mChannelsPerFrame;
    return 1;
fail:
    ExtAudioFileDispose(a->file);
    a->file = NULL;
    return 0;
}

static void sys_close(sys_audio *a)
{
    if (a->file)
        ExtAudioFileDispose(a->file);
    a->file = NULL;
}

static size_t sys_read(sys_audio *a, float *out, size_t frames)
{
    size_t got = 0;

    while (got < frames) {
        AudioBufferList bl;
        UInt32 n = (UInt32)(frames - got);
        bl.mNumberBuffers = 1;
        bl.mBuffers[0].mNumberChannels = a->channels;
        bl.mBuffers[0].mDataByteSize = n * 4 * a->channels;
        bl.mBuffers[0].mData = out + got * a->channels;
        if (ExtAudioFileRead(a->file, &n, &bl) != noErr || n == 0)
            break;
        got += n;
    }
    return got;
}

static void sys_title(const char *path, char *title, size_t cap)
{
    AudioFileID af = NULL;
    CFDictionaryRef info = NULL;
    UInt32 size = sizeof info;
    CFURLRef url = sys_url(path);

    if (!url)
        return;
    if (AudioFileOpenURL(url, kAudioFileReadPermission, 0, &af) == noErr && af) {
        if (AudioFileGetProperty(af, kAudioFilePropertyInfoDictionary, &size, &info) == noErr && info) {
            CFStringRef t = (CFStringRef)CFDictionaryGetValue(info, CFSTR(kAFInfoDictionary_Title));
            if (t && CFGetTypeID(t) == CFStringGetTypeID())
                CFStringGetCString(t, title, (CFIndex)cap, kCFStringEncodingUTF8);
            CFRelease(info);
        }
        AudioFileClose(af);
    }
    CFRelease(url);
}

static int sys_probe(const char *path, uint32_t *ms, char *title, size_t cap)
{
    sys_audio a;

    if (!sys_open(&a, path))
        return 0;
    if (a.rate)
        *ms = (uint32_t)(a.frames * 1000 / a.rate);
    sys_close(&a);
    if (cap > 1)
        sys_title(path, title, cap);
    return 1;
}
#endif /* __APPLE__ */

/* ── flac, ogg, wav: the decoders' own headers ─────────────────────── */

typedef struct { char *title; size_t cap; } meta_ctx;

static int comment_is_title(const char *c, size_t n)
{
    static const char key[] = "title=";
    size_t i;
    if (n < 6)
        return 0;
    for (i = 0; i < 6; i++)
        if (tolower((unsigned char)c[i]) != key[i])
            return 0;
    return 1;
}

static void flac_meta(void *user, drflac_metadata *m)
{
    meta_ctx *ctx = (meta_ctx *)user;
    drflac_vorbis_comment_iterator it;
    const char *c;
    drflac_uint32 n;

    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT || ctx->title[0])
        return;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount,
                                        m->data.vorbis_comment.pComments);
    while ((c = drflac_next_vorbis_comment(&it, &n)) != NULL)
        if (comment_is_title(c, n)) {
            copy_text(ctx->title, ctx->cap, c + 6, n - 6);
            break;
        }
}

static stb_vorbis *ogg_open(const char *path)
{
    FILE *f = open_file(path);
    stb_vorbis *v;
    int err = 0;

    if (!f)
        return NULL;
    /* close_on_free: the file is the stream's, and a stream that does not
     * open has closed it already (vorbis_deinit). Closing it again here was
     * a second fclose of a freed FILE: on glibc that closes its descriptor
     * number again, which by then may be a file another thread opened. */
    v = stb_vorbis_open_file(f, 1, &err, NULL);
    return v;
}

int xst_probe(const char *path, uint32_t *ms, char *title, size_t title_cap)
{
    const char *why;
    return xst_probe_why(path, ms, title, title_cap, &why);
}

int xst_probe_why(const char *path, uint32_t *ms, char *title, size_t title_cap, const char **why)
{
    uint32_t dummy;
    const char *unused;

    if (!why)
        why = &unused;
    *why = "";
    if (!ms)
        ms = &dummy;
    *ms = 0;
    if (title && title_cap)
        title[0] = 0;
    else {
        static char none[1];
        title = none; title_cap = 1;
    }

    switch (kind_of(path)) {
    case K_MP3:
        return mp3_probe(path, ms, title, title_cap, why);
    case K_FLAC: {
        meta_ctx ctx;
        drflac *fl;
#if defined(_WIN32)
        wchar_t w[1024];
#endif
        ctx.title = title; ctx.cap = title_cap;
#if defined(_WIN32)
        fl = wide_path(path, w, 1024) ? drflac_open_file_with_metadata_w(w, flac_meta, &ctx, NULL) : NULL;
#else
        fl = drflac_open_file_with_metadata(path, flac_meta, &ctx, NULL);
#endif
        if (!fl) {
            *why = "not a FLAC file this can read";
            return 0;
        }
        if (fl->sampleRate)
            *ms = (uint32_t)(fl->totalPCMFrameCount * 1000 / fl->sampleRate);
        drflac_close(fl);
        return 1;
    }
    case K_OGG: {
        stb_vorbis *v = ogg_open(path);
        stb_vorbis_info info;
        stb_vorbis_comment c;
        int i;
        if (!v) {
            *why = "not an Ogg Vorbis file this can read";
            return 0;
        }
        info = stb_vorbis_get_info(v);
        c = stb_vorbis_get_comment(v);
        for (i = 0; i < c.comment_list_length; i++) {
            size_t n = strlen(c.comment_list[i]);
            if (comment_is_title(c.comment_list[i], n)) {
                copy_text(title, title_cap, c.comment_list[i] + 6, n - 6);
                break;
            }
        }
        if (info.sample_rate)
            *ms = (uint32_t)((uint64_t)stb_vorbis_stream_length_in_samples(v) * 1000
                             / info.sample_rate);
        stb_vorbis_close(v);
        return 1;
    }
    case K_WAV: {
        drwav w;
#if defined(_WIN32)
        wchar_t wp[1024];
        if (!wide_path(path, wp, 1024) || !drwav_init_file_w(&w, wp, NULL)) {
            *why = "not a WAV file this can read";
            return 0;
        }
#else
        if (!drwav_init_file(&w, path, NULL)) {
            *why = "not a WAV file this can read";
            return 0;
        }
#endif
        if (w.sampleRate)
            *ms = (uint32_t)(w.totalPCMFrameCount * 1000 / w.sampleRate);
        drwav_uninit(&w);
        return 1;
    }
#if defined(__APPLE__)
    case K_SYSTEM:
        if (!sys_probe(path, ms, title, title_cap)) {
            *why = "the system cannot decode it (a protected file?)";
            return 0;
        }
        return 1;
#endif
    default:
        *why = "not a kind of file this plays";
        return 0;
    }
}

/* ── the stream ────────────────────────────────────────────────────── */

/*
 * Resampling to 44.1 kHz: a Kaiser-windowed sinc, RS_TAPS taps at RS_PHASES
 * positions between two input frames, the two nearest positions blended.
 * The cutoff sits just under the lower of the two Nyquist rates, so 48 kHz
 * and 96 kHz files do not alias. A 44.1 kHz file is passed through.
 */
#define RS_TAPS    32
#define RS_PHASES  256
#define RS_HALF    (RS_TAPS / 2)
#define RS_INBUF   4096                   /* frames */

struct xst_stream {
    xst_kind    kind;
    drmp3       mp3;
    drflac     *flac;
    drwav       wav;
    stb_vorbis *ogg;
#if defined(__APPLE__)
    sys_audio   sys;
#endif
    unsigned    rate, channels;
    int         src_eof;

    int         resample;
    float      *taps;                     /* (RS_PHASES + 1) * RS_TAPS */
    float       in[(RS_INBUF + RS_TAPS) * 2];
    size_t      in_len;                   /* frames in `in` */
    double      pos, step;                /* the next output frame, in input frames */
    size_t      tail;                     /* zero frames appended after the end */
    int         done;
    float      *wide;                     /* scratch for files with > 2 channels */
};

static double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    int k;
    for (k = 1; k < 40; k++) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

static int make_taps(xst_stream *s)
{
    const double pi = 3.14159265358979323846, beta = 9.0;
    double cut = s->rate > XST_RATE ? (double)XST_RATE / s->rate : 1.0;
    int p, k;

    cut *= 0.94;
    s->taps = (float *)malloc(sizeof(float) * (RS_PHASES + 1) * RS_TAPS);
    if (!s->taps)
        return 0;
    for (p = 0; p <= RS_PHASES; p++) {
        double frac = (double)p / RS_PHASES, sum = 0, h[RS_TAPS];
        for (k = 0; k < RS_TAPS; k++) {
            double t = (double)(k - (RS_HALF - 1)) - frac;       /* input frames */
            double w = t / RS_HALF, v;
            v = fabs(t) < 1e-9 ? cut : sin(pi * cut * t) / (pi * t);
            w = fabs(w) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - w * w)) / bessel_i0(beta);
            h[k] = v * w;
            sum += h[k];
        }
        for (k = 0; k < RS_TAPS; k++)
            s->taps[p * RS_TAPS + k] = (float)(h[k] / sum);
    }
    return 1;
}

/* Up to `frames` stereo float frames from the file; 0 at its end. */
static size_t src_read(xst_stream *s, float *out, size_t frames)
{
    size_t got = 0, i;
    float *buf = out;
    unsigned ch = s->channels;

    if (s->src_eof || !frames)
        return 0;
    if (ch > 2) {
        if (!s->wide)
            s->wide = (float *)malloc(sizeof(float) * RS_INBUF * ch);
        if (!s->wide)
            return 0;
        if (frames > RS_INBUF)
            frames = RS_INBUF;
        buf = s->wide;
    }
    switch (s->kind) {
    case K_MP3:  got = (size_t)drmp3_read_pcm_frames_f32(&s->mp3, frames, buf); break;
    case K_FLAC: got = (size_t)drflac_read_pcm_frames_f32(s->flac, frames, buf); break;
    case K_WAV:  got = (size_t)drwav_read_pcm_frames_f32(&s->wav, frames, buf); break;
    case K_OGG:
        got = (size_t)stb_vorbis_get_samples_float_interleaved(s->ogg, (int)ch, buf,
                                                               (int)(frames * ch));
        break;
#if defined(__APPLE__)
    case K_SYSTEM: got = sys_read(&s->sys, buf, frames); break;
#endif
    default: break;
    }
    if (got < frames)
        s->src_eof = 1;
    if (ch == 1) {
        for (i = got; i-- > 0;)
            out[i * 2] = out[i * 2 + 1] = out[i];
    } else if (ch > 2) {
        /* The front pair; a centre channel (3.0, 5.1) goes to both. */
        for (i = 0; i < got; i++) {
            float c = (ch == 3 || ch >= 5) ? buf[i * ch + 2] * 0.7071f : 0.0f;
            out[i * 2] = buf[i * ch] + c;
            out[i * 2 + 1] = buf[i * ch + 1] + c;
        }
    }
    return got;
}

static int16_t to_s16(float v)
{
    v *= 32767.0f;
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)lrintf(v);
}

xst_stream *xst_stream_open(const char *path)
{
    xst_stream *s = (xst_stream *)calloc(1, sizeof *s);
    int ok = 0;
#if defined(_WIN32)
    wchar_t w[1024];
    int have_w = wide_path(path, w, 1024);
#endif

    if (!s)
        return NULL;
    s->kind = kind_of(path);
    switch (s->kind) {
    case K_MP3:
#if defined(_WIN32)
        ok = have_w && drmp3_init_file_w(&s->mp3, w, NULL);
#else
        ok = drmp3_init_file(&s->mp3, path, NULL);
#endif
        if (ok) { s->rate = s->mp3.sampleRate; s->channels = s->mp3.channels; }
        break;
    case K_FLAC:
#if defined(_WIN32)
        s->flac = have_w ? drflac_open_file_w(w, NULL) : NULL;
#else
        s->flac = drflac_open_file(path, NULL);
#endif
        ok = s->flac != NULL;
        if (ok) { s->rate = s->flac->sampleRate; s->channels = s->flac->channels; }
        break;
    case K_WAV:
#if defined(_WIN32)
        ok = have_w && drwav_init_file_w(&s->wav, w, NULL);
#else
        ok = drwav_init_file(&s->wav, path, NULL);
#endif
        if (ok) { s->rate = s->wav.sampleRate; s->channels = s->wav.channels; }
        break;
    case K_OGG:
        s->ogg = ogg_open(path);
        ok = s->ogg != NULL;
        if (ok) {
            stb_vorbis_info info = stb_vorbis_get_info(s->ogg);
            s->rate = info.sample_rate; s->channels = (unsigned)info.channels;
        }
        break;
#if defined(__APPLE__)
    case K_SYSTEM:
        ok = sys_open(&s->sys, path);
        if (ok) { s->rate = s->sys.rate; s->channels = s->sys.channels; }
        break;
#endif
    default:
        break;
    }
    if (!ok) {
        s->kind = K_NONE;
        xst_stream_close(s);
        return NULL;
    }
    if (s->rate < 4000 || s->rate > 384000 || s->channels < 1 || s->channels > 8) {
        xst_stream_close(s);
        return NULL;
    }
    s->resample = s->rate != XST_RATE;
    if (s->resample) {
        if (!make_taps(s)) {
            xst_stream_close(s);
            return NULL;
        }
        s->step = (double)s->rate / XST_RATE;
        /* RS_HALF - 1 frames of silence ahead of the first, for the taps
         * that reach back. */
        s->in_len = RS_HALF - 1;
        s->pos = RS_HALF - 1;
    }
    return s;
}

void xst_stream_close(xst_stream *s)
{
    if (!s)
        return;
    switch (s->kind) {
    case K_MP3:  drmp3_uninit(&s->mp3); break;
    case K_FLAC: if (s->flac) drflac_close(s->flac); break;
    case K_WAV:  drwav_uninit(&s->wav); break;
    case K_OGG:  if (s->ogg) stb_vorbis_close(s->ogg); break;
#if defined(__APPLE__)
    case K_SYSTEM: sys_close(&s->sys); break;
#endif
    default: break;
    }
    free(s->taps);
    free(s->wide);
    free(s);
}

void xst_stream_format(const xst_stream *s, unsigned *rate, unsigned *channels)
{
    if (rate) *rate = s->rate;
    if (channels) *channels = s->channels;
}

/* Keep the frames the taps of `pos` still reach, read more behind them. */
static void rs_refill(xst_stream *s)
{
    size_t keep_from = (size_t)s->pos - (RS_HALF - 1), room, got;

    if (keep_from > 0) {
        memmove(s->in, s->in + keep_from * 2, (s->in_len - keep_from) * 2 * sizeof(float));
        s->in_len -= keep_from;
        s->pos -= (double)keep_from;
    }
    room = RS_INBUF + RS_TAPS - s->in_len;
    got = src_read(s, s->in + s->in_len * 2, room);
    s->in_len += got;
    if (got == 0 && s->tail < RS_HALF) {
        /* The end: silence for the taps that reach ahead of the last frame. */
        size_t z = RS_HALF - s->tail;
        if (z > room)
            z = room;
        memset(s->in + s->in_len * 2, 0, z * 2 * sizeof(float));
        s->in_len += z;
        s->tail += z;
    }
}

size_t xst_stream_read(xst_stream *s, int16_t *out, size_t frames)
{
    size_t n = 0;

    if (!s || s->done)
        return 0;
    if (!s->resample) {
        while (n < frames) {
            size_t want = frames - n, got, i;
            if (want > RS_INBUF)
                want = RS_INBUF;
            got = src_read(s, s->in, want);
            for (i = 0; i < got * 2; i++)
                out[n * 2 + i] = to_s16(s->in[i]);
            n += got;
            if (got < want)
                break;
        }
        if (n < frames)
            s->done = 1;
        return n;
    }
    while (n < frames) {
        size_t ip = (size_t)s->pos;
        const float *x, *h0, *h1;
        double fr;
        float f, l = 0, r = 0;
        int ph, k;

        if (ip + RS_HALF >= s->in_len) {
            /* Past the last real frame: the song is over. */
            if (s->src_eof && s->tail >= RS_HALF) {
                s->done = 1;
                break;
            }
            rs_refill(s);
            continue;
        }
        fr = (s->pos - (double)ip) * RS_PHASES;
        ph = (int)fr;
        f = (float)(fr - ph);
        h0 = s->taps + ph * RS_TAPS;
        h1 = h0 + RS_TAPS;
        x = s->in + (ip - (RS_HALF - 1)) * 2;
        for (k = 0; k < RS_TAPS; k++) {
            float h = h0[k] + (h1[k] - h0[k]) * f;
            l += x[k * 2] * h;
            r += x[k * 2 + 1] * h;
        }
        out[n * 2] = to_s16(l);
        out[n * 2 + 1] = to_s16(r);
        n++;
        s->pos += s->step;
    }
    return n;
}

/* kernel_ps2save.c - see kernel_ps2save.h. */
#include "kernel_ps2save.h"
#include "recomp_save_dir.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS_MAX_FILE   (32u * 1024u * 1024u)   /* a card is 8.25 MB; a save file far less */
#define PS_MAX_UNPACK (16u * 1024u * 1024u)   /* what one save file may unpack to */
#define PS_DIRENT     512u

/* Directory entry mode bits on the card. */
#define DF_FILE   0x0010u
#define DF_DIR    0x0020u
#define DF_EXISTS 0x8000u

static void ps_err(char *err, unsigned n, const char *fmt, ...)
{
    va_list ap;
    if (!err || !n)
        return;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static unsigned rd16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* A fixed-size name field, ended at its first zero. */
static void name_of(const uint8_t *p, size_t max, char *out, size_t n)
{
    size_t i;
    for (i = 0; i < max && i + 1 < n && p[i]; i++)
        out[i] = (char)p[i];
    out[i] = 0;
}

static const char *leaf(const char *p)
{
    const char *s = strrchr(p, '/'), *b = strrchr(p, '\\');
    if (b && (!s || b > s))
        s = b;
    return s ? s + 1 : p;
}

/* ── the save itself ──────────────────────────────────────────────── */

uint32_t ps2save_sum(const uint8_t *d, size_t n)
{
    uint32_t s = 0;
    size_t i;
    for (i = 0; i + 4 <= n; i += 4)
        s += rd32(d + i);
    return s;
}

int ps2save_data_ok(const uint8_t *d, size_t n)
{
    return d && n == PS2SAVE_DATA_SIZE && rd32(d) == 0x10 &&
           ps2save_sum(d, PS2SAVE_DATA_SIZE - 4) == rd32(d + PS2SAVE_DATA_SIZE - 4);
}

#define PROF_AT(i)  (0xE374u + (unsigned)(i) * 0x49Cu)
#define PROF_LEN    0x49Cu
#define XB_SUM_AT   0xE810u

/* UTF-16 name (8 characters and a zero at +8) to UTF-8. */
static void prof_name(const uint8_t *p, char *out, size_t n)
{
    size_t o = 0;
    int i;
    for (i = 0; i < 9; i++) {
        unsigned c = rd16(p + 8 + 2 * i);
        if (!c)
            break;
        if (c < 0x80) {
            if (o + 1 >= n)
                break;
            out[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= n)
                break;
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= n)
                break;
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    while (o > 0 && out[o - 1] == ' ')
        o--;
    out[o] = 0;
}

static void describe(ps2save_b3 *s)
{
    int i, k;
    s->used = 0;
    for (i = 0; i < PS2SAVE_SLOTS; i++) {
        const uint8_t *p = s->data + PROF_AT(i);
        ps2save_profile *q = &s->prof[i];
        float secs;
        uint32_t bits = rd32(p + 4);
        memset(q, 0, sizeof *q);
        q->used = p[PROF_LEN - 1] == 0;
        memcpy(&secs, &bits, 4);
        q->hours = secs > 0 && secs < 1e9f ? secs / 3600.0f : 0;
        prof_name(p, q->name, sizeof q->name);
        for (k = 0; k < 173; k++) {
            uint8_t m = p[0x386 + k];
            if (m == 3)
                q->gold++;
            if (m >= 1 && m <= 3)
                q->medals++;
        }
        if (q->used)
            s->used++;
    }
}

int ps2save_to_xbox(const uint8_t ps2[PS2SAVE_DATA_SIZE], int slot, const uint8_t *settings,
                    uint8_t out[PS2SAVE_XBOX_SIZE])
{
    if (slot < 0 || slot >= PS2SAVE_SLOTS || ps2[PROF_AT(slot) + PROF_LEN - 1] != 0)
        return 0;
    memset(out, 0, PS2SAVE_XBOX_SIZE);
    memcpy(out, ps2, PS2SAVE_SETTINGS);                     /* version and records table */
    if (settings) {
        memcpy(out + PS2SAVE_SETTINGS, settings, PS2SAVE_SETTINGS_LEN);
    } else {
        memcpy(out + PS2SAVE_SETTINGS, ps2 + PS2SAVE_SETTINGS, PS2SAVE_SETTINGS_LEN);
        /* +0x30 and +0x31 move the picture on a PS2's TV (the game scales
         * them into the screen offset); here they stay at the middle. */
        out[PS2SAVE_SETTINGS + 0x30] = 0;
        out[PS2SAVE_SETTINGS + 0x31] = 0;
    }
    memcpy(out + PROF_AT(0), ps2 + PROF_AT(slot), PROF_LEN);
    wr32(out + XB_SUM_AT, ps2save_sum(out, XB_SUM_AT));
    return 1;
}

/* ── what was found ───────────────────────────────────────────────── */

typedef struct {
    ps2save_b3 *out;
    int found;
    char bad[200];          /* the first damaged Burnout 3 save seen */
} Hit;

/* A file of a save folder: `dir` the folder's name, `name` the file's. */
static void consider(Hit *h, const char *dir, const char *name, const uint8_t *d, size_t n,
                     const char *kind, const char *file)
{
    if (strcmp(dir, name) != 0 || strlen(dir) < 10 || dir[0] != 'B')
        return;
    if (!ps2save_data_ok(d, n)) {
        if (n == PS2SAVE_DATA_SIZE && !h->bad[0])
            snprintf(h->bad, sizeof h->bad, "The Burnout 3 save %s in %s is damaged (its check sum is wrong).",
                     dir, leaf(file));
        return;
    }
    if (h->found++ == 0) {
        memcpy(h->out->data, d, n);
        snprintf(h->out->dir, sizeof h->out->dir, "%s", dir);
        snprintf(h->out->kind, sizeof h->out->kind, "%s", kind);
        snprintf(h->out->file, sizeof h->out->file, "%s", file);
    }
}

/* ── LZARI (Okumura 1989), the decoder only ───────────────────────── */

#define LZ_N      4096
#define LZ_F      60
#define LZ_TH     2
#define LZ_NCHAR  (256 - LZ_TH + LZ_F)
#define LZ_Q1     (1u << 15)
#define LZ_Q2     (2u * LZ_Q1)
#define LZ_Q3     (3u * LZ_Q1)
#define LZ_Q4     (4u * LZ_Q1)
#define LZ_MAXCUM (LZ_Q1 - 1)

typedef struct {
    const uint8_t *in;
    size_t inlen, bit;
    uint32_t low, high, value;
    int sym_to_char[LZ_NCHAR + 1];
    unsigned sym_freq[LZ_NCHAR + 1], sym_cum[LZ_NCHAR + 1], pos_cum[LZ_N + 1];
} Lz;

static unsigned lz_bit(Lz *z)
{
    size_t b = z->bit++;
    if (b / 8 >= z->inlen)
        return 0;
    return (z->in[b / 8] >> (7 - b % 8)) & 1u;
}

static void lz_update(Lz *z, int sym)
{
    int i, c;
    if (z->sym_cum[0] >= LZ_MAXCUM) {
        c = 0;
        for (i = LZ_NCHAR; i > 0; i--) {
            z->sym_cum[i] = (unsigned)c;
            z->sym_freq[i] = (z->sym_freq[i] + 1) >> 1;
            c += (int)z->sym_freq[i];
        }
        z->sym_cum[0] = (unsigned)c;
    }
    for (i = sym; z->sym_freq[i] == z->sym_freq[i - 1]; i--)
        ;
    if (i < sym) {
        int a = z->sym_to_char[i], b = z->sym_to_char[sym];
        z->sym_to_char[i] = b;
        z->sym_to_char[sym] = a;
    }
    z->sym_freq[i]++;
    while (--i >= 0)
        z->sym_cum[i]++;
}

static void lz_norm(Lz *z)
{
    for (;;) {
        if (z->low >= LZ_Q2) {
            z->value -= LZ_Q2;
            z->low -= LZ_Q2;
            z->high -= LZ_Q2;
        } else if (z->low >= LZ_Q1 && z->high <= LZ_Q3) {
            z->value -= LZ_Q1;
            z->low -= LZ_Q1;
            z->high -= LZ_Q1;
        } else if (z->high > LZ_Q2) {
            break;
        }
        z->low += z->low;
        z->high += z->high;
        z->value = 2 * z->value + lz_bit(z);
    }
}

static int lz_char(Lz *z)
{
    uint64_t range = z->high - z->low, x;
    int i = 1, j = LZ_NCHAR, k, ch;
    if (!range || !z->sym_cum[0])
        return -1;
    x = (((uint64_t)z->value - z->low + 1) * z->sym_cum[0] - 1) / range;
    while (i < j) {
        k = (i + j) / 2;
        if (z->sym_cum[k] > x)
            i = k + 1;
        else
            j = k;
    }
    z->high = z->low + (uint32_t)(range * z->sym_cum[i - 1] / z->sym_cum[0]);
    z->low += (uint32_t)(range * z->sym_cum[i] / z->sym_cum[0]);
    lz_norm(z);
    ch = z->sym_to_char[i];
    lz_update(z, i);
    return ch;
}

static int lz_pos(Lz *z)
{
    uint64_t range = z->high - z->low, x;
    int i = 1, j = LZ_N, k;
    if (!range)
        return -1;
    x = (((uint64_t)z->value - z->low + 1) * z->pos_cum[0] - 1) / range;
    while (i < j) {
        k = (i + j) / 2;
        if (z->pos_cum[k] > x)
            i = k + 1;
        else
            j = k;
    }
    i--;
    z->high = z->low + (uint32_t)(range * z->pos_cum[i] / z->pos_cum[0]);
    z->low += (uint32_t)(range * z->pos_cum[i + 1] / z->pos_cum[0]);
    lz_norm(z);
    return i;
}

/* 1 when `in` unpacks to `outlen` bytes. */
static int lzari_decode(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    Lz *z = (Lz *)calloc(1, sizeof(Lz));
    uint8_t text[LZ_N];
    size_t o = 0;
    int i, r = LZ_N - LZ_F, ok = 1;
    if (!z)
        return 0;
    z->in = in;
    z->inlen = inlen;
    z->high = LZ_Q4;
    z->sym_cum[LZ_NCHAR] = 0;
    for (i = LZ_NCHAR; i >= 1; i--) {
        z->sym_to_char[i] = i - 1;
        z->sym_freq[i] = 1;
        z->sym_cum[i - 1] = z->sym_cum[i] + 1;
    }
    z->sym_freq[0] = 0;
    z->pos_cum[LZ_N] = 0;
    for (i = LZ_N; i >= 1; i--)
        z->pos_cum[i - 1] = z->pos_cum[i] + 10000u / (unsigned)(i + 200);
    for (i = 0; i < 17; i++)
        z->value = 2 * z->value + lz_bit(z);
    memset(text, ' ', sizeof text);
    while (o < outlen) {
        int c = lz_char(z);
        if (c < 0 || z->bit > inlen * 8 + 64) {     /* ran past the data: damaged */
            ok = 0;
            break;
        }
        if (c < 256) {
            out[o++] = (uint8_t)c;
            text[r] = (uint8_t)c;
            r = (r + 1) & (LZ_N - 1);
        } else {
            int p = lz_pos(z), len = c - 255 + LZ_TH, s, k;
            if (p < 0) {
                ok = 0;
                break;
            }
            s = (r - p - 1) & (LZ_N - 1);
            for (k = 0; k < len && o < outlen; k++) {
                uint8_t b = text[(s + k) & (LZ_N - 1)];
                out[o++] = b;
                text[r] = b;
                r = (r + 1) & (LZ_N - 1);
            }
        }
    }
    free(z);
    return ok && o == outlen;
}

/* ── the single-save files ────────────────────────────────────────── */

/* .max: "Ps2PowerSave", CRC, folder name, title, packed size, file count,
 * unpacked size; LZARI data of (u32 size, name[32], data) records, each
 * record starting 8 bytes before a 16-byte boundary. */
static int read_max(Hit *h, const uint8_t *b, size_t n, const char *file, char *err, unsigned es)
{
    char dir[40], name[40];
    uint32_t clen, count, len, i;
    size_t off = 0, avail;
    uint8_t *u;
    if (n < 0x5C)
        return 0;
    name_of(b + 0x10, 32, dir, sizeof dir);
    clen = rd32(b + 0x50);
    count = rd32(b + 0x54);
    len = rd32(b + 0x58);
    if (len == 0 || len > PS_MAX_UNPACK || count == 0 || count > 1000) {
        ps_err(err, es, "%s is not a readable Action Replay MAX save.", leaf(file));
        return -1;
    }
    avail = n - 0x5C;
    if (clen != len && clen >= 4 && clen - 4 < avail)
        avail = clen - 4;
    u = (uint8_t *)malloc(len);
    if (!u)
        return -1;
    if (!lzari_decode(b + 0x5C, avail, u, len)) {
        free(u);
        ps_err(err, es, "%s is damaged (its packed data does not unpack).", leaf(file));
        return -1;
    }
    for (i = 0; i < count; i++) {
        uint32_t fl;
        if (off + 36 > len)
            break;
        fl = rd32(u + off);
        name_of(u + off + 4, 32, name, sizeof name);
        off += 36;
        if (fl > len - off)
            break;
        consider(h, dir, name, u + off, fl, "Action Replay MAX save (.max)", file);
        off += fl;
        off = ((off + 8 + 15) & ~(size_t)15) - 8;
    }
    free(u);
    return 1;
}

/* .cbs: "CFU\0", ?, header size, unpacked size, body size, folder name, ...;
 * the body is RC4 (a fixed key state, no key schedule) over zlib data of
 * (64-byte header: times, size at +16, name at +32; data) records. */
static const uint8_t k_cbs_state[256] = {
    0x5f, 0x1f, 0x85, 0x6f, 0x31, 0xaa, 0x3b, 0x18, 0x21, 0xb9, 0xce, 0x1c, 0x07, 0x4c, 0x9c, 0xb4,
    0x81, 0xb8, 0xef, 0x98, 0x59, 0xae, 0xf9, 0x26, 0xe3, 0x80, 0xa3, 0x29, 0x2d, 0x73, 0x51, 0x62,
    0x7c, 0x64, 0x46, 0xf4, 0x34, 0x1a, 0xf6, 0xe1, 0xba, 0x3a, 0x0d, 0x82, 0x79, 0x0a, 0x5c, 0x16,
    0x71, 0x49, 0x8e, 0xac, 0x8c, 0x9f, 0x35, 0x19, 0x45, 0x94, 0x3f, 0x56, 0x0c, 0x91, 0x00, 0x0b,
    0xd7, 0xb0, 0xdd, 0x39, 0x66, 0xa1, 0x76, 0x52, 0x13, 0x57, 0xf3, 0xbb, 0x4e, 0xe5, 0xdc, 0xf0,
    0x65, 0x84, 0xb2, 0xd6, 0xdf, 0x15, 0x3c, 0x63, 0x1d, 0x89, 0x14, 0xbd, 0xd2, 0x36, 0xfe, 0xb1,
    0xca, 0x8b, 0xa4, 0xc6, 0x9e, 0x67, 0x47, 0x37, 0x42, 0x6d, 0x6a, 0x03, 0x92, 0x70, 0x05, 0x7d,
    0x96, 0x2f, 0x40, 0x90, 0xc4, 0xf1, 0x3e, 0x3d, 0x01, 0xf7, 0x68, 0x1e, 0xc3, 0xfc, 0x72, 0xb5,
    0x54, 0xcf, 0xe7, 0x41, 0xe4, 0x4d, 0x83, 0x55, 0x12, 0x22, 0x09, 0x78, 0xfa, 0xde, 0xa7, 0x06,
    0x08, 0x23, 0xbf, 0x0f, 0xcc, 0xc1, 0x97, 0x61, 0xc5, 0x4a, 0xe6, 0xa0, 0x11, 0xc2, 0xea, 0x74,
    0x02, 0x87, 0xd5, 0xd1, 0x9d, 0xb7, 0x7e, 0x38, 0x60, 0x53, 0x95, 0x8d, 0x25, 0x77, 0x10, 0x5e,
    0x9b, 0x7f, 0xd8, 0x6e, 0xda, 0xa2, 0x2e, 0x20, 0x4f, 0xcd, 0x8f, 0xcb, 0xbe, 0x5a, 0xe0, 0xed,
    0x2c, 0x9a, 0xd4, 0xe2, 0xaf, 0xd0, 0xa9, 0xe8, 0xad, 0x7a, 0xbc, 0xa8, 0xf2, 0xee, 0xeb, 0xf5,
    0xa6, 0x99, 0x28, 0x24, 0x6c, 0x2b, 0x75, 0x5d, 0xf8, 0xd3, 0x86, 0x17, 0xfb, 0xc0, 0x7b, 0xb3,
    0x58, 0xdb, 0xc7, 0x4b, 0xff, 0x04, 0x50, 0xe9, 0x88, 0x69, 0xc9, 0x2a, 0xab, 0xfd, 0x5b, 0x1b,
    0x8a, 0xd9, 0xec, 0x27, 0x44, 0x0e, 0x33, 0xc8, 0x6b, 0x93, 0x32, 0x48, 0xb6, 0x30, 0x43, 0xa5,
};

static int read_cbs(Hit *h, const uint8_t *b, size_t n, const char *file, char *err, unsigned es)
{
    uint32_t hlen, dlen, flen;
    size_t blen, off = 0;
    uint8_t s[256], *body, *u;
    unsigned i, j = 0, k;
    char dir[40], name[40];
    if (n < 0x5C)
        return 0;
    hlen = rd32(b + 8);
    dlen = rd32(b + 12);
    flen = rd32(b + 16);
    name_of(b + 20, 32, dir, sizeof dir);
    if (hlen < 124 || hlen >= n || dlen == 0 || dlen > PS_MAX_UNPACK) {
        ps_err(err, es, "%s is not a readable CodeBreaker save.", leaf(file));
        return -1;
    }
    blen = n - hlen;
    if (flen < blen)
        blen = flen;                   /* else the size given is the whole file's */
    body = (uint8_t *)malloc(blen);
    u = (uint8_t *)malloc(dlen);
    if (!body || !u) {
        free(body);
        free(u);
        return -1;
    }
    memcpy(s, k_cbs_state, sizeof s);
    for (k = 0; k < blen; k++) {
        uint8_t t;
        i = (k + 1) & 255;
        j = (j + s[i]) & 255;
        t = s[i];
        s[i] = s[j];
        s[j] = t;
        body[k] = b[hlen + k] ^ s[(s[i] + s[j]) & 255];
    }
    /* zlib: a two-byte header, then raw deflate. */
    if (blen < 6 || (body[0] & 0x0F) != 8 || ((unsigned)body[0] << 8 | body[1]) % 31 != 0 ||
        !xbox_SaveImportInflate(body + 2, blen - 2, u, dlen)) {
        free(body);
        free(u);
        ps_err(err, es, "%s is damaged (its packed data does not unpack).", leaf(file));
        return -1;
    }
    free(body);
    while (off + 64 <= dlen) {
        uint32_t fl = rd32(u + off + 16);
        name_of(u + off + 32, 32, name, sizeof name);
        off += 64;
        if (fl > dlen - off)
            break;
        consider(h, dir, name, u + off, fl, "CodeBreaker save (.cbs)", file);
        off += fl;
    }
    free(u);
    return 1;
}

/* .xps/.sps: 0x0D "SharkPortSave", type, three length-prefixed strings,
 * size; then a 98-byte (or longer) header for the folder (name[64] at +2,
 * entry count at +66) and one per file (size at +66) followed by its data. */
static int read_sharkport(Hit *h, const uint8_t *b, size_t n, const char *file, char *err, unsigned es)
{
    size_t off = 21;
    unsigned hl, count, i;
    char dir[70], name[70];
    for (i = 0; i < 3; i++) {
        if (off + 4 > n || rd32(b + off) > n - off - 4)
            goto bad;
        off += 4 + rd32(b + off);
    }
    off += 4;
    if (off + 98 > n)
        goto bad;
    hl = rd16(b + off);
    name_of(b + off + 2, 64, dir, sizeof dir);
    count = rd32(b + off + 66);
    if (hl < 98 || hl > n - off || count < 2 || count > 1000)
        goto bad;
    off += hl;
    for (i = 0; i + 2 < count; i++) {
        uint32_t fl;
        if (off + 98 > n)
            goto bad;
        hl = rd16(b + off);
        name_of(b + off + 2, 64, name, sizeof name);
        fl = rd32(b + off + 66);
        if (hl < 98 || hl > n - off || fl > n - off - hl)
            goto bad;
        off += hl;
        consider(h, dir, name, b + off, fl, "SharkPort / X-Port save (.sps/.xps)", file);
        off += fl;
    }
    return 1;
bad:
    ps_err(err, es, "%s is damaged (a SharkPort/X-Port save cut short).", leaf(file));
    return -1;
}

/* .psv: "\0VSP", version 0, signature, ..., type 2 (a PS2 save) at 0x3C; a
 * 40-byte header with the file count at 0x64; the folder's 56-byte entry
 * (name at +24); then per file a 56-byte entry (size at +16, name at +24)
 * and the file's offset. */
static int read_psv(Hit *h, const uint8_t *b, size_t n, const char *file, char *err, unsigned es)
{
    uint32_t count, i;
    char dir[40], name[40];
    size_t e;
    if (n < 0xA0 || rd32(b + 4) != 0 || rd32(b + 0x3C) != 2)
        return 0;
    count = rd32(b + 0x40 + 36);
    name_of(b + 0x68 + 24, 32, dir, sizeof dir);
    if (count > 1000 || 0xA0 + (size_t)count * 60 > n) {
        ps_err(err, es, "%s is damaged (a PS3 save of a PS2 game cut short).", leaf(file));
        return -1;
    }
    for (i = 0, e = 0xA0; i < count; i++, e += 60) {
        uint32_t fl = rd32(b + e + 16), at = rd32(b + e + 56);
        name_of(b + e + 24, 32, name, sizeof name);
        if (at > n || fl > n - at)
            continue;
        consider(h, dir, name, b + at, fl, "PS3 copy of a PS2 save (.psv)", file);
    }
    return 1;
}

/* .psu: 512-byte directory entries: the folder (its length counts "." and
 * ".."), ".", "..", then per file its entry and its data padded to 1 KB. */
static int looks_psu(const uint8_t *b, size_t n)
{
    unsigned m0, m1, m2;
    if (n < 3 * PS_DIRENT)
        return 0;
    m0 = rd16(b);
    m1 = rd16(b + PS_DIRENT);
    m2 = rd16(b + 2 * PS_DIRENT);
    return (m0 & DF_DIR) && (m1 & DF_DIR) && (m2 & DF_DIR) && rd32(b + 4) >= 2 &&
           !strcmp((const char *)b + PS_DIRENT + 0x40, ".") && !strcmp((const char *)b + 2 * PS_DIRENT + 0x40, "..");
}

static int read_psu(Hit *h, const uint8_t *b, size_t n, const char *file, char *err, unsigned es)
{
    char dir[40], name[40];
    uint32_t count = rd32(b + 4) - 2, i;
    size_t off = 3 * PS_DIRENT;
    name_of(b + 0x40, 32, dir, sizeof dir);
    for (i = 0; i < count; i++) {
        uint32_t fl;
        if (off + PS_DIRENT > n)
            goto bad;
        fl = rd32(b + off + 4);
        name_of(b + off + 0x40, 32, name, sizeof name);
        off += PS_DIRENT;
        if (fl > n - off)
            goto bad;
        if (rd16(b + off - PS_DIRENT) & DF_FILE)
            consider(h, dir, name, b + off, fl, "uLaunchELF/EMS save (.psu)", file);
        off += (fl + 1023u) & ~(size_t)1023u;
    }
    return 1;
bad:
    ps_err(err, es, "%s is damaged (a .psu save cut short).", leaf(file));
    return -1;
}

/* ── the memory card ──────────────────────────────────────────────── */

/* Superblock: "Sony PS2 Memory Card Format ", page size at 0x28, pages per
 * cluster 0x2A, clusters per card 0x30, first allocatable cluster 0x34, end
 * 0x38, root folder cluster 0x3C, the indirect FAT cluster list at 0x50.
 * A page is followed by 16 spare (ECC) bytes in a full image; a cluster's
 * FAT entry is found through two levels (indirect list, FAT cluster); an
 * entry has bit 31 set when in use, 0xFFFFFFFF ends a chain. */
typedef struct {
    const uint8_t *b;
    size_t n;
    unsigned page, ppc, raw, csize, epc;
    uint32_t clusters, alloc_off, alloc_end, root;
    uint32_t ifc[32];
} Card;

static const uint8_t *card_cluster(const Card *c, uint32_t abs, uint8_t *buf)
{
    unsigned i;
    for (i = 0; i < c->ppc; i++) {
        size_t at = ((size_t)abs * c->ppc + i) * c->raw;
        if (at + c->page > c->n)
            return NULL;
        memcpy(buf + (size_t)i * c->page, c->b + at, c->page);
    }
    return buf;
}

/* The FAT entry of allocatable cluster `k`; 0 when it cannot be read. */
static int card_fat(const Card *c, uint32_t k, uint32_t *v, uint8_t *buf)
{
    uint32_t fc = k / c->epc, ind;
    if (k >= c->alloc_end || fc / c->epc >= 32)
        return 0;
    if (!card_cluster(c, c->ifc[fc / c->epc], buf))
        return 0;
    ind = rd32(buf + 4 * (fc % c->epc));
    if (!card_cluster(c, ind, buf))
        return 0;
    *v = rd32(buf + 4 * (k % c->epc));
    return 1;
}

/* The bytes of a chain from allocatable cluster `first`, `len` of them. */
static uint8_t *card_read(const Card *c, uint32_t first, size_t len)
{
    uint8_t *out, *buf = (uint8_t *)malloc(c->csize), *fat = (uint8_t *)malloc(c->csize);
    size_t got = 0;
    uint32_t k = first, steps = 0;
    out = (uint8_t *)malloc(len ? len : 1);
    if (!out || !buf || !fat || len > PS_MAX_FILE)
        goto fail;
    while (got < len) {
        uint32_t v;
        size_t take = len - got < c->csize ? len - got : c->csize;
        if (++steps > c->clusters || !card_cluster(c, k + c->alloc_off, buf))
            goto fail;
        memcpy(out + got, buf, take);
        got += take;
        if (got >= len)
            break;
        if (!card_fat(c, k, &v, fat) || v == 0xFFFFFFFFu || !(v & 0x80000000u))
            goto fail;
        k = v & 0x7FFFFFFFu;
    }
    free(buf);
    free(fat);
    return out;
fail:
    free(out);
    free(buf);
    free(fat);
    return NULL;
}

static int read_card(Hit *h, const uint8_t *b, size_t n, const char *file, char *err, unsigned es)
{
    Card c;
    uint8_t *root = NULL;
    uint32_t count, i;
    size_t plain, full;
    memset(&c, 0, sizeof c);
    c.b = b;
    c.n = n;
    c.page = rd16(b + 0x28);
    c.ppc = rd16(b + 0x2A);
    c.clusters = rd32(b + 0x30);
    c.alloc_off = rd32(b + 0x34);
    c.alloc_end = rd32(b + 0x38);
    c.root = rd32(b + 0x3C);
    for (i = 0; i < 32; i++)
        c.ifc[i] = rd32(b + 0x50 + 4 * i);
    if (c.page < 512 || c.page > 8192 || (c.page & 127) || c.ppc == 0 || c.ppc > 16 || c.clusters == 0 ||
        c.clusters > (1u << 22)) {
        ps_err(err, es, "%s is a PS2 memory card image this importer cannot read.", leaf(file));
        return -1;
    }
    c.csize = c.page * c.ppc;
    c.epc = c.csize / 4;
    plain = (size_t)c.clusters * c.csize;
    full = (size_t)c.clusters * c.ppc * (c.page + c.page / 128 * 4);
    c.raw = n >= full ? c.page + c.page / 128 * 4 : c.page;       /* with the ECC bytes or without */
    if (n < plain) {
        ps_err(err, es, "%s is cut short (%u bytes; a card of this size has %u).", leaf(file), (unsigned)n,
               (unsigned)plain);
        return -1;
    }
    /* The root folder: its first entry (".") holds its length. */
    {
        uint8_t *first = card_read(&c, c.root, PS_DIRENT);
        if (!first || !(rd16(first) & DF_DIR) || (count = rd32(first + 4)) < 2 || count > 65536) {
            free(first);
            ps_err(err, es, "%s: the memory card's folder list is damaged.", leaf(file));
            return -1;
        }
        free(first);
    }
    root = card_read(&c, c.root, (size_t)count * PS_DIRENT);
    if (!root) {
        ps_err(err, es, "%s: the memory card's folder list is damaged.", leaf(file));
        return -1;
    }
    for (i = 2; i < count; i++) {
        const uint8_t *e = root + (size_t)i * PS_DIRENT;
        uint32_t sub_n, j;
        uint8_t *sub;
        char dir[40];
        if ((rd16(e) & (DF_DIR | DF_EXISTS)) != (DF_DIR | DF_EXISTS))
            continue;
        name_of(e + 0x40, 32, dir, sizeof dir);
        sub_n = rd32(e + 4);
        if (sub_n < 2 || sub_n > 4096 || !(sub = card_read(&c, rd32(e + 0x10), (size_t)sub_n * PS_DIRENT)))
            continue;
        for (j = 2; j < sub_n; j++) {
            const uint8_t *f = sub + (size_t)j * PS_DIRENT;
            char name[40];
            uint32_t fl = rd32(f + 4);
            uint8_t *d;
            if ((rd16(f) & (DF_FILE | DF_EXISTS)) != (DF_FILE | DF_EXISTS))
                continue;
            name_of(f + 0x40, 32, name, sizeof name);
            if (strcmp(name, dir) || fl != PS2SAVE_DATA_SIZE)
                continue;
            d = card_read(&c, rd32(f + 0x10), fl);
            if (d) {
                consider(h, dir, name, d, fl, "PS2 memory card image", file);
                free(d);
            }
        }
        free(sub);
    }
    free(root);
    return 1;
}

/* ── a file of any kind ───────────────────────────────────────────── */

static int read_any(Hit *h, const uint8_t *b, size_t n, const char *file, char *err, unsigned es)
{
    if (n >= 28 && !memcmp(b, "Sony PS2 Memory Card Format ", 28))
        return read_card(h, b, n, file, err, es);
    if (n >= 12 && !memcmp(b, "Ps2PowerSave", 12))
        return read_max(h, b, n, file, err, es);
    if (n >= 4 && !memcmp(b, "CFU\0", 4))
        return read_cbs(h, b, n, file, err, es);
    if (n >= 17 && !memcmp(b, "\x0d\0\0\0SharkPortSave", 17))
        return read_sharkport(h, b, n, file, err, es);
    if (n >= 4 && !memcmp(b, "\0VSP", 4))
        return read_psv(h, b, n, file, err, es);
    if (looks_psu(b, n))
        return read_psu(h, b, n, file, err, es);
    return 0;
}

static int finish(Hit *h, const char *where, char *err, unsigned es)
{
    if (h->found) {
        h->out->found = h->found;
        describe(h->out);
        if (!h->out->used) {
            ps_err(err, es, "The Burnout 3 save %s in %s has no profile in it.", h->out->dir, leaf(h->out->file));
            return 0;
        }
        return 1;
    }
    if (h->bad[0])
        ps_err(err, es, "%s", h->bad);
    else if (err && es && !err[0])
        ps_err(err, es, "No Burnout 3 PS2 save in %s.", where);
    return 0;
}

int ps2save_read_buffer(const uint8_t *buf, size_t n, const char *name, ps2save_b3 *out,
                        char *err, unsigned err_size)
{
    Hit h;
    memset(&h, 0, sizeof h);
    h.out = out;
    if (err && err_size)
        err[0] = 0;
    if (read_any(&h, buf, n, name, err, err_size) < 0 && !h.found)
        return 0;
    return finish(&h, name, err, err_size);
}

/* ── looking through folders ──────────────────────────────────────── */

static uint8_t *ps_read_file(const char *p, size_t *n, size_t max)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b;
    long sz;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || (size_t)sz > max || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    b = (uint8_t *)malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static int ext_is(const char *name, const char *const *list)
{
    const char *dot = strrchr(name, '.');
    int i;
    if (!dot)
        return 0;
    for (i = 0; list[i]; i++) {
        const char *a = dot + 1, *b = list[i];
        for (; *a && *b && tolower((unsigned char)*a) == *b; a++, b++)
            ;
        if (!*a && !*b)
            return 1;
    }
    return 0;
}

/* The endings PS2 save files and card images come with. A file is only
 * read when its ending is one of these (or a save folder's data file). */
static const char *const k_exts[] = { "ps2", "mc2", "mcd", "mcr", "bin", "vmc", "max", "cbs", "xps", "sps",
                                      "psv", "psu", "npo", NULL };

typedef struct {
    Hit *h;
    const ps2save_bound *bd;
    int depth, out;
    char *err;
    unsigned es;
} Walk;

static int walk_dir(Walk *w, const char *dir, int depth);

static int spent(Walk *w)
{
    if (!w->out && w->bd && w->bd->spent && w->bd->spent(w->bd->ctx))
        w->out = 1;
    return w->out;
}

typedef struct {
    Walk *w;
    const char *dir;
    int depth;
    int pass;               /* 0: files, 1: folders */
} Each;

static int each_cb(const char *dir, const char *name, int dir_entry, void *vctx)
{
    Each *e = (Each *)vctx;
    Walk *w = e->w;
    char p[1024];
    if (spent(w) || w->h->found)
        return 0;
    if (name[0] == '.' || !strcmp(name, "__MACOSX") || strlen(dir) + strlen(name) + 2 >= sizeof p)
        return 1;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (e->pass == 0 && !dir_entry) {
        size_t n = 0;
        uint8_t *b;
        char sub_err[300] = "";
        const char *folder = leaf(dir);     /* '/' or '\\' (a Windows path picked in the browser) */
        if (!strcmp(folder, name)) {
            /* The data file of a save folder (a PCSX2 folder card, a copy). */
            if ((b = ps_read_file(p, &n, PS2SAVE_DATA_SIZE)) != NULL) {
                consider(w->h, folder, name, b, n, "PS2 save folder", dir);
                free(b);
            }
            return 1;
        }
        if (!ext_is(name, k_exts) || !(b = ps_read_file(p, &n, PS_MAX_FILE)))
            return 1;
        if (read_any(w->h, b, n, p, sub_err, sizeof sub_err) < 0 && sub_err[0] && w->err && !w->err[0])
            ps_err(w->err, w->es, "%s", sub_err);
        free(b);
        return 1;
    }
    if (e->pass == 1 && dir_entry && e->depth > 0)
        walk_dir(w, p, e->depth - 1);
    return 1;
}

static int walk_dir(Walk *w, const char *dir, int depth)
{
    Each e;
    if (spent(w))
        return 0;
    e.w = w;
    e.dir = dir;
    e.depth = depth;
    e.pass = 0;
    recomp_fs_each(dir, each_cb, &e);
    if (w->h->found || w->out || depth <= 0)
        return w->h->found;
    if (w->bd && w->bd->shallow && w->bd->shallow(dir, w->bd->ctx))
        return 0;
    e.pass = 1;
    recomp_fs_each(dir, each_cb, &e);
    return w->h->found;
}

int ps2save_find(const char *path, int depth, const ps2save_bound *bound, ps2save_b3 *out,
                 char *err, unsigned err_size)
{
    Hit h;
    Walk w;
    memset(&h, 0, sizeof h);
    h.out = out;
    if (err && err_size)
        err[0] = 0;
    if (!path || !*path)
        return 0;
    if (!recomp_fs_is_dir(path)) {
        size_t n = 0;
        uint8_t *b = ps_read_file(path, &n, PS_MAX_FILE);
        int r;
        if (!b) {
            ps_err(err, err_size, "%s could not be read.", path);
            return 0;
        }
        r = read_any(&h, b, n, path, err, err_size);
        free(b);
        if (r == 0) {
            ps_err(err, err_size, "%s is not a PS2 save file or memory card image.", leaf(path));
            return 0;
        }
        return finish(&h, path, err, err_size);
    }
    memset(&w, 0, sizeof w);
    w.h = &h;
    w.bd = bound;
    w.err = err;
    w.es = err_size;
    {
        /* Without a trailing separator: a save folder is known by its name. */
        char top[1024];
        size_t l;
        snprintf(top, sizeof top, "%s", path);
        for (l = strlen(top); l > 1 && (top[l - 1] == '/' || top[l - 1] == '\\'); l--)
            top[l - 1] = 0;
        walk_dir(&w, top, depth);
    }
    if (!h.found && w.out)
        return -1;
    if (!h.found && err && err_size && err[0] && !h.bad[0])
        return 0;               /* a damaged file was the reason */
    return finish(&h, path, err, err_size);
}

/* kernel_console_keys.c - see kernel_console_keys.h. */
#include "kernel_console_keys.h"
#include "kernel.h"
#include "recomp_env.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static char s_status[200] = "Console key: not set (saves are signed with zero keys)";

const char *xbox_ConsoleKeyStatus(void) { return s_status; }

int xbox_ConsoleKeyParseHex(const char *text, uint8_t out[16])
{
    int n = 0, hi = -1;
    if (!text)
        return 0;
    for (; *text; text++) {
        int v;
        if (isspace((unsigned char)*text) || *text == ':' || *text == '-')
            continue;
        if (*text >= '0' && *text <= '9') v = *text - '0';
        else if (*text >= 'a' && *text <= 'f') v = *text - 'a' + 10;
        else if (*text >= 'A' && *text <= 'F') v = *text - 'A' + 10;
        else return 0;
        if (hi < 0) hi = v;
        else {
            if (n >= 16) return 0;
            out[n++] = (uint8_t)(hi << 4 | v);
            hi = -1;
        }
    }
    return n == 16 && hi < 0;
}

/* ── SHA-1 from a given state (the EEPROM's "key" is an HMAC whose two
 * midstates are fixed per console version, see below) ─────────────────── */

static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a, b, c, d, e;
    int t;
    for (t = 0; t < 16; t++)
        w[t] = (uint32_t)p[4*t] << 24 | (uint32_t)p[4*t+1] << 16 | (uint32_t)p[4*t+2] << 8 | p[4*t+3];
    for (; t < 80; t++)
        w[t] = rol(w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16], 1);
    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];
    for (t = 0; t < 80; t++) {
        uint32_t f, k, tmp;
        if (t < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (t < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        tmp = rol(a, 5) + f + e + k + w[t];
        e = d; d = c; c = rol(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

/* SHA-1 of data (len <= 55 + 64k) continuing from `iv` as if 64 bytes had
 * been hashed already. */
static void sha1_from(const uint32_t iv[5], const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5];
    uint8_t blk[128];
    size_t full = len / 64, rem = len % 64, i, nb;
    uint64_t bits = ((uint64_t)len + 64) * 8;
    memcpy(h, iv, sizeof h);
    for (i = 0; i < full; i++)
        sha1_block(h, data + 64 * i);
    memset(blk, 0, sizeof blk);
    memcpy(blk, data + 64 * full, rem);
    blk[rem] = 0x80;
    nb = rem < 56 ? 1 : 2;
    for (i = 0; i < 8; i++)
        blk[64 * nb - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (i = 0; i < nb; i++)
        sha1_block(h, blk + 64 * i);
    for (i = 0; i < 5; i++) {
        out[4*i] = (uint8_t)(h[i] >> 24); out[4*i+1] = (uint8_t)(h[i] >> 16);
        out[4*i+2] = (uint8_t)(h[i] >> 8); out[4*i+3] = (uint8_t)h[i];
    }
}

/* The EEPROM key of each retail console revision, as the SHA-1 midstates of
 * its HMAC (ipad block, opad block). Public (xboxdevwiki, xbeeprom). */
static const uint32_t s_ipad[4][5] = {
    { 0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4 },
    { 0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC },
    { 0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED },
    { 0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C },
};
static const uint32_t s_opad[4][5] = {
    { 0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3 },
    { 0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8 },
    { 0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393 },
    { 0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609 },
};

static void eeprom_hmac(int v, const uint8_t *data, size_t len, uint8_t out[20])
{
    uint8_t inner[20];
    sha1_from(s_ipad[v], data, len, inner);
    sha1_from(s_opad[v], inner, 20, out);
}

static void rc4(const uint8_t *key, size_t klen, uint8_t *buf, size_t n)
{
    uint8_t S[256], t;
    unsigned i, j = 0;
    size_t k;
    for (i = 0; i < 256; i++) S[i] = (uint8_t)i;
    for (i = 0; i < 256; i++) {
        j = (j + S[i] + key[i % klen]) & 255;
        t = S[i]; S[i] = S[j]; S[j] = t;
    }
    i = j = 0;
    for (k = 0; k < n; k++) {
        i = (i + 1) & 255; j = (j + S[i]) & 255;
        t = S[i]; S[i] = S[j]; S[j] = t;
        buf[k] ^= S[(S[i] + S[j]) & 255];
    }
}

/* 0x00 hash(20), 0x14 confounder(8), 0x1C HDD key(16), 0x2C region(4): the
 * last 28 bytes are RC4-encrypted with HMAC(key, hash); the hash is the HMAC
 * of the decrypted 28 bytes. The revision whose hash checks out is the one. */
int xbox_ConsoleKeyFromEeprom(const uint8_t eeprom[256], uint8_t hdkey[16])
{
    int v;
    for (v = 0; v < 4; v++) {
        uint8_t rk[20], dec[28], chk[20];
        eeprom_hmac(v, eeprom, 20, rk);
        memcpy(dec, eeprom + 0x14, 28);
        rc4(rk, 20, dec, 28);
        eeprom_hmac(v, dec, 28, chk);
        if (!memcmp(chk, eeprom, 20)) {
            memcpy(hdkey, dec + 8, 16);
            return 1;
        }
    }
    return 0;
}

void xbox_ConsoleKeyDeriveSignature(const uint8_t cert_key[16], uint8_t out[16])
{
    /* The kernel's fixed key; the same constant every signing tool uses. */
    static const uint8_t master[16] = { 0x5C,0x07,0x33,0xAE,0x04,0x01,0xF7,0xE8,
                                        0xBA,0x79,0x93,0xFD,0xCD,0x2F,0x1F,0xE0 };
    uint8_t d[20];
    xbox_XcHMAC(master, 16, cert_key, 16, NULL, 0, d);
    memcpy(out, d, 16);
}

int xbox_ConsoleKeyLoad(uint8_t hdkey[16])
{
    const char *hex = recomp_env(RENV_CONSOLE_HDKEY);
    const char *path = recomp_env(RENV_CONSOLE_EEPROM);
    if (hex && *hex) {
        if (!xbox_ConsoleKeyParseHex(hex, hdkey)) {
            snprintf(s_status, sizeof s_status,
                     "Console key: not used, the HDD key is not 32 hex digits");
            return 0;
        }
        snprintf(s_status, sizeof s_status, "Console key: set (an HDD key)");
        return 1;
    }
    if (path && *path) {
        uint8_t e[256];
        size_t n = 0;
        FILE *f = fopen(path, "rb");
        if (f) { n = fread(e, 1, sizeof e, f); fclose(f); }
        if (n != 256) {
            snprintf(s_status, sizeof s_status,
                     "Console key: not used, %.120s is not a 256-byte eeprom.bin", path);
            return 0;
        }
        if (!xbox_ConsoleKeyFromEeprom(e, hdkey)) {
            snprintf(s_status, sizeof s_status,
                     "Console key: not used, that eeprom.bin does not decrypt (retail consoles only)");
            return 0;
        }
        snprintf(s_status, sizeof s_status, "Console key: set (from an eeprom.bin)");
        return 1;
    }
    return 0;
}

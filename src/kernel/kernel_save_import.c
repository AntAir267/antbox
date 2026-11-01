/* kernel_save_import.c - see kernel_save_import.h. */
#include "kernel_save_import.h"
#include "kernel.h"
#include "kernel_console_keys.h"
#include "recomp_save_dir.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SI_PATH 1024
#define SI_MAX_FILE (64u * 1024u * 1024u)       /* one file of a zip */
#define SI_MAX_ZIP  (512u * 1024u * 1024u)      /* the zip itself */
#define SI_MAX_TOTAL (256u * 1024u * 1024u)     /* all it unpacks to */
#define SI_WORK "import-work"

static const char k_profile[] = "Profile 1";
static const char k_meta[] = "SaveMeta.xbx";
static const char *const k_title_files[] = { "TitleMeta.xbx", "TitleImage.xbx", "SaveImage.xbx" };

static void si_err(char *err, unsigned n, const char *fmt, ...)
{
    va_list ap;
    if (!err || !n)
        return;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static int ci_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return !*a && !*b;
}

static int ends_with_ci(const char *s, const char *suffix)
{
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && ci_eq(s + a - b, suffix);
}

static int is_hex8(const char *s)
{
    int i;
    for (i = 0; i < 8; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return s[8] == 0;
}

/* The last component of a path, and the path without it. */
static const char *leaf_of(const char *p)
{
    const char *e = p + strlen(p), *b;
    while (e > p && (e[-1] == '/' || e[-1] == '\\'))
        e--;
    b = e;
    while (b > p && b[-1] != '/' && b[-1] != '\\')
        b--;
    return b;
}

static void leaf_copy(const char *p, char *out, size_t n)
{
    const char *b = leaf_of(p), *e = p + strlen(p);
    size_t len;
    while (e > b && (e[-1] == '/' || e[-1] == '\\'))
        e--;
    len = (size_t)(e - b);
    if (len >= n)
        len = n - 1;
    memcpy(out, b, len);
    out[len] = 0;
}

static void parent_copy(const char *p, char *out, size_t n)
{
    const char *b = leaf_of(p);
    size_t len = (size_t)(b - p);
    while (len > 1 && (p[len - 1] == '/' || p[len - 1] == '\\'))
        len--;
    if (len >= n)
        len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static uint8_t *read_file(const char *p, size_t *n, size_t max)
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

static int write_file(const char *p, const uint8_t *d, size_t n)
{
    FILE *f = fopen(p, "wb");
    int ok;
    if (!f)
        return 0;
    ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/* ── inflate (RFC 1951; after zlib's puff) ─────────────────────────── */

typedef struct {
    const uint8_t *in;
    size_t inlen, incnt;
    unsigned bitbuf, bitcnt;
    uint8_t *out;
    size_t outlen, outcnt;
} Inf;

typedef struct {
    short count[16];
    short symbol[288];
} Huff;

static int inf_bits(Inf *s, int need)
{
    long val = (long)s->bitbuf;
    while ((int)s->bitcnt < need) {
        if (s->incnt >= s->inlen)
            return -1;
        val |= (long)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (unsigned)(val >> need);
    s->bitcnt -= (unsigned)need;
    return (int)(val & ((1L << need) - 1));
}

static int inf_decode(Inf *s, const Huff *h)
{
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= 15; len++) {
        int b = inf_bits(s, 1);
        if (b < 0)
            return -1;
        code |= b;
        count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static int inf_construct(Huff *h, const short *length, int n)
{
    int symbol, len, left;
    short offs[16];
    for (len = 0; len <= 15; len++)
        h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++)
        h->count[length[symbol]]++;
    if (h->count[0] == n)
        return 0;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int inf_codes(Inf *s, const Huff *lencode, const Huff *distcode)
{
    static const short lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const short dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    int symbol;
    do {
        symbol = inf_decode(s, lencode);
        if (symbol < 0)
            return 1;
        if (symbol < 256) {
            if (s->outcnt >= s->outlen)
                return 1;
            s->out[s->outcnt++] = (uint8_t)symbol;
        } else if (symbol > 256) {
            int len, dist, b;
            symbol -= 257;
            if (symbol >= 29)
                return 1;
            b = inf_bits(s, lext[symbol]);
            if (b < 0)
                return 1;
            len = lens[symbol] + b;
            symbol = inf_decode(s, distcode);
            if (symbol < 0 || symbol >= 30)
                return 1;
            b = inf_bits(s, dext[symbol]);
            if (b < 0)
                return 1;
            dist = dists[symbol] + b;
            if ((size_t)dist > s->outcnt || s->outcnt + (size_t)len > s->outlen)
                return 1;
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - (size_t)dist];
                s->outcnt++;
            }
        }
    } while (symbol != 256);
    return 0;
}

static int inf_fixed(Inf *s)
{
    static Huff lencode, distcode;
    static int built;
    if (!built) {
        short lengths[288];
        int symbol;
        for (symbol = 0; symbol < 144; symbol++) lengths[symbol] = 8;
        for (; symbol < 256; symbol++) lengths[symbol] = 9;
        for (; symbol < 280; symbol++) lengths[symbol] = 7;
        for (; symbol < 288; symbol++) lengths[symbol] = 8;
        inf_construct(&lencode, lengths, 288);
        for (symbol = 0; symbol < 30; symbol++) lengths[symbol] = 5;
        inf_construct(&distcode, lengths, 30);
        built = 1;
    }
    return inf_codes(s, &lencode, &distcode);
}

static int inf_dynamic(Inf *s)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen, ndist, ncode, index, err;
    short lengths[320];
    Huff lencode, distcode;

    nlen = inf_bits(s, 5);
    ndist = inf_bits(s, 5);
    ncode = inf_bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return 1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30)
        return 1;
    for (index = 0; index < ncode; index++) {
        int b = inf_bits(s, 3);
        if (b < 0)
            return 1;
        lengths[order[index]] = (short)b;
    }
    for (; index < 19; index++)
        lengths[order[index]] = 0;
    err = inf_construct(&lencode, lengths, 19);
    if (err != 0)
        return 1;
    index = 0;
    while (index < nlen + ndist) {
        int symbol = inf_decode(s, &lencode), len, rep;
        if (symbol < 0)
            return 1;
        if (symbol < 16) {
            lengths[index++] = (short)symbol;
        } else {
            len = 0;
            if (symbol == 16) {
                if (index == 0)
                    return 1;
                len = lengths[index - 1];
                rep = inf_bits(s, 2);
                if (rep < 0) return 1;
                rep += 3;
            } else if (symbol == 17) {
                rep = inf_bits(s, 3);
                if (rep < 0) return 1;
                rep += 3;
            } else {
                rep = inf_bits(s, 7);
                if (rep < 0) return 1;
                rep += 11;
            }
            if (index + rep > nlen + ndist)
                return 1;
            while (rep--)
                lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0)
        return 1;
    err = inf_construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
        return 1;
    err = inf_construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
        return 1;
    return inf_codes(s, &lencode, &distcode);
}

/* 1 when `in` inflates to exactly `outlen` bytes. */
static int inflate_raw(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen)
{
    Inf s;
    int last, type, err = 0;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.inlen = inlen;
    s.out = out;
    s.outlen = outlen;
    do {
        last = inf_bits(&s, 1);
        type = inf_bits(&s, 2);
        if (last < 0 || type < 0)
            return 0;
        if (type == 0) {
            unsigned len, nlen;
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.incnt + 4 > s.inlen)
                return 0;
            len = s.in[s.incnt] | (unsigned)s.in[s.incnt + 1] << 8;
            nlen = s.in[s.incnt + 2] | (unsigned)s.in[s.incnt + 3] << 8;
            s.incnt += 4;
            if (len != (~nlen & 0xFFFF) || s.incnt + len > s.inlen || s.outcnt + len > s.outlen)
                return 0;
            memcpy(s.out + s.outcnt, s.in + s.incnt, len);
            s.incnt += len;
            s.outcnt += len;
        } else if (type == 1) {
            err = inf_fixed(&s);
        } else if (type == 2) {
            err = inf_dynamic(&s);
        } else {
            return 0;
        }
        if (err)
            return 0;
    } while (!last);
    return s.outcnt == outlen;
}

static uint32_t crc32_of(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= d[i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* ── zip ───────────────────────────────────────────────────────────── */

static unsigned rd16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* A path inside a zip that is safe to join under a folder: relative, no
 * "..", no drive letter or colon, no empty parts. Backslashes become '/'. */
static int zip_name_ok(char *name)
{
    char *p;
    size_t i, part = 0;
    for (p = name; *p; p++)
        if (*p == '\\')
            *p = '/';
    if (!*name || name[0] == '/' || strchr(name, ':'))
        return 0;
    for (i = 0;; i++) {
        if (name[i] == '/' || !name[i]) {
            if (part == 0 || (part == 2 && name[i - 1] == '.' && name[i - 2] == '.'))
                return 0;
            part = 0;
            if (!name[i])
                break;
        } else {
            part++;
        }
    }
    return 1;
}

/* Unpack `zip` under `dest`. Entries of macOS resource forks are skipped. */
static int unzip_to(const char *zip, const char *dest, char *err, unsigned err_size)
{
    size_t zn = 0, i, total = 0;
    uint8_t *z = read_file(zip, &zn, SI_MAX_ZIP);
    size_t eocd = 0, cd, count;
    int ok = 1, any = 0;

    if (!z) {
        si_err(err, err_size, "%s could not be read as a zip file.", zip);
        return 0;
    }
    if (zn >= 22) {
        for (i = zn - 22;; i--) {
            if (rd32(z + i) == 0x06054b50u) {
                eocd = i;
                break;
            }
            if (i == 0 || zn - i > 65557)
                break;
        }
    }
    if (!eocd) {
        free(z);
        si_err(err, err_size, "%s is not a zip file (or is damaged).", zip);
        return 0;
    }
    count = rd16(z + eocd + 10);
    cd = rd32(z + eocd + 16);
    for (i = 0; i < count && ok; i++) {
        const uint8_t *e;
        unsigned method, nlen, elen, clen, lnlen, lelen, flags;
        uint32_t csize, usize, lho, crc;
        char name[SI_PATH], path[SI_PATH * 2];
        uint8_t *out;
        size_t pos;

        if (cd + 46 > zn || rd32(z + cd) != 0x02014b50u) {
            ok = 0;
            break;
        }
        e = z + cd;
        flags = rd16(e + 8);
        method = rd16(e + 10);
        crc = rd32(e + 16);
        csize = rd32(e + 20);
        usize = rd32(e + 24);
        nlen = rd16(e + 28);
        elen = rd16(e + 30);
        clen = rd16(e + 32);
        lho = rd32(e + 42);
        if (cd + 46 + nlen + elen + clen > zn || nlen >= sizeof name) {
            ok = 0;
            break;
        }
        memcpy(name, e + 46, nlen);
        name[nlen] = 0;
        cd += 46 + nlen + elen + clen;
        if (name[nlen ? nlen - 1 : 0] == '/' || !strncmp(name, "__MACOSX/", 9) || strstr(name, "/._") || !strncmp(name, "._", 2))
            continue;                          /* a folder entry, or a resource fork */
        if (!zip_name_ok(name))
            continue;                          /* never writes outside `dest` */
        if ((flags & 1) || (method != 0 && method != 8) || usize > SI_MAX_FILE ||
            lho + 30 > zn || rd32(z + lho) != 0x04034b50u) {
            si_err(err, err_size, "%s holds a file this importer cannot read (%s): encrypted, or packed another way.", zip, name);
            ok = 0;
            break;
        }
        total += usize;
        if (total > SI_MAX_TOTAL) {
            si_err(err, err_size, "%s unpacks to more than %u MB: it is not a save.", zip, SI_MAX_TOTAL >> 20);
            ok = 0;
            break;
        }
        lnlen = rd16(z + lho + 26);
        lelen = rd16(z + lho + 28);
        pos = (size_t)lho + 30 + lnlen + lelen;
        if (pos + csize > zn) {
            ok = 0;
            break;
        }
        out = (uint8_t *)malloc(usize ? usize : 1);
        if (!out) {
            ok = 0;
            break;
        }
        if ((method == 0 ? (csize == usize && (memcpy(out, z + pos, usize), 1)) : inflate_raw(z + pos, csize, out, usize)) &&
            crc32_of(out, usize) == crc) {
            char *slash;
            snprintf(path, sizeof path, "%s/%s", dest, name);
            slash = strrchr(path, '/');
            if (slash) {
                *slash = 0;
                recomp_fs_mkdir_p(path);
                *slash = '/';
            }
            if (!write_file(path, out, usize)) {
                si_err(err, err_size, "The files of %s could not be written under %s.", zip, dest);
                ok = 0;
            }
            any = 1;
        } else {
            si_err(err, err_size, "%s is damaged (%s did not unpack).", zip, name);
            ok = 0;
        }
        free(out);
    }
    free(z);
    if (ok && !any) {
        si_err(err, err_size, "%s is empty.", zip);
        return 0;
    }
    if (!ok && (!err || !err[0]))
        si_err(err, err_size, "%s is damaged.", zip);
    return ok;
}

/* ── finding the save ──────────────────────────────────────────────── */

/* How much a check may look at (BO-289): every entry counts, the clock
 * and the caller's stop are asked at each one, and a folder on a slow
 * volume is looked at but not below. NULL (an import): no bound. */
#define SI_CHECK_ENTRIES 50000
#define SI_CHECK_MS      3000

typedef struct {
    const xbox_save_check_limits *l;
    long     entries;
    uint64_t deadline;          /* ms */
    int      out;               /* ran out or was stopped: the answer is not known */
    int      skipped;           /* a folder was not looked below */
} Lim;

static uint64_t si_now_ms(void)
{
#if defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* One more entry looked at; 1 when the looking has to stop. */
static int lim_spent(Lim *b)
{
    if (!b)
        return 0;
    if (!b->out && (--b->entries < 0 || si_now_ms() >= b->deadline
                    || (b->l && b->l->stop && b->l->stop(b->l->ctx))))
        b->out = 1;
    return b->out;
}

typedef struct {
    const char *want;
    int want_dir;
    Lim *b;
    char found[SI_PATH];
} Named;

static int named_cb(const char *dir, const char *name, int dir_entry, void *vctx)
{
    Named *n = (Named *)vctx;
    (void)dir;
    if (lim_spent(n->b))
        return 0;
    if (!!dir_entry == !!n->want_dir && ci_eq(name, n->want) && strlen(name) < sizeof n->found) {
        snprintf(n->found, sizeof n->found, "%s", name);
        return 0;
    }
    return 1;
}

/* The entry of `dir` called `want` in any case; path in `out`. */
static int find_named_in(const char *dir, const char *want, int want_dir, char *out, size_t n, Lim *b)
{
    Named c;
    c.want = want;
    c.want_dir = want_dir;
    c.b = b;
    c.found[0] = 0;
    recomp_fs_each(dir, named_cb, &c);
    if (!c.found[0])
        return 0;
    snprintf(out, n, "%s/%s", dir, c.found);
    return 1;
}

static int find_named(const char *dir, const char *want, int want_dir, char *out, size_t n)
{
    return find_named_in(dir, want, want_dir, out, n, NULL);
}

typedef struct {
    int depth;
    Lim *b;
    char found[SI_PATH];
} Search;

static int search_save(const char *dir, int depth, char *out, size_t n, Lim *b);

static int search_cb(const char *dir, const char *name, int dir_entry, void *vctx)
{
    Search *s = (Search *)vctx;
    char p[SI_PATH];
    if (lim_spent(s->b))
        return 0;
    if (!dir_entry || name[0] == '.' || !strcmp(name, "__MACOSX") || strlen(dir) + strlen(name) + 2 >= sizeof p)
        return 1;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (search_save(p, s->depth - 1, s->found, sizeof s->found, s->b))
        return 0;
    return 1;
}

/* A folder with SaveMeta.xbx and the profile, `dir` itself or below it. */
static int search_save(const char *dir, int depth, char *out, size_t n, Lim *b)
{
    char p[SI_PATH];
    Search s;
    if (lim_spent(b))
        return 0;
    if (find_named_in(dir, k_meta, 0, p, sizeof p, b) && find_named_in(dir, k_profile, 0, p, sizeof p, b)) {
        snprintf(out, n, "%s", dir);
        return 1;
    }
    if (depth <= 0 || (b && b->out))
        return 0;
    if (b && b->l && b->l->shallow && b->l->shallow(dir, b->l->ctx)) {
        b->skipped = 1;                    /* a network share: not walked */
        return 0;
    }
    s.depth = depth;
    s.b = b;
    s.found[0] = 0;
    recomp_fs_each(dir, search_cb, &s);
    if (!s.found[0])
        return 0;
    snprintf(out, n, "%s", s.found);
    return 1;
}

/* The save folder under `root` (a folder), and what the paths say about its
 * title: 1 when the path names another game (then `err` says so). */
static int locate(const char *root, const char *title_id, char *save, size_t save_n, int *have_tid,
                  char *err, unsigned err_size, Lim *b)
{
    char parent[SI_PATH], pl[SI_PATH];
    *have_tid = 0;
    if (!search_save(root, 4, save, save_n, b)) {
        si_err(err, err_size, "No Burnout 3 save in %s. Expected a folder with SaveMeta.xbx and \"Profile 1\" in it "
               "(UDATA/%s/<save id>/ on the Xbox), or a folder or zip that holds it.", root, title_id);
        return 0;
    }
    parent_copy(save, parent, sizeof parent);
    leaf_copy(parent, pl, sizeof pl);
    if (is_hex8(pl)) {
        if (!ci_eq(pl, title_id)) {
            si_err(err, err_size, "This save belongs to another game (title id %s). Burnout 3 is %s.", pl, title_id);
            return 0;
        }
        *have_tid = 1;
    }
    return 1;
}

/* ── the profile ───────────────────────────────────────────────────── */

/* The player's name: the first run of letters, digits, spaces and - _ in
 * UTF-16 (3 to 15 of them, ended by a zero) in the profile. Best effort,
 * for the report only. */
static void profile_name(const uint8_t *d, size_t n, char *out, size_t out_n)
{
    size_t i, j;
    out[0] = 0;
    for (i = 0; i + 8 <= n; i += 2) {
        for (j = 0; i + 2 * j + 1 < n && j < 16; j++) {
            unsigned c = rd16(d + i + 2 * j);
            if (!(c < 128 && (isalnum((int)c) || c == ' ' || c == '-' || c == '_')))
                break;
        }
        if (j >= 3 && j <= 15 && i + 2 * j + 1 < n && rd16(d + i + 2 * j) == 0 && (i < 2 || rd16(d + i - 2) == 0)) {
            size_t k;
            for (k = 0; k < j && k + 1 < out_n; k++)
                out[k] = (char)rd16(d + i + 2 * k);
            out[k] = 0;
            return;
        }
    }
}

/* signature = HMAC(hd, HMAC(sig, data minus the last 20 bytes)). */
static void sign_profile(uint8_t *d, size_t n, const uint8_t hd[16], const uint8_t sig[16])
{
    uint8_t inner[20];
    xbox_XcHMAC(sig, 16, d, (ULONG)(n - 20), NULL, 0, inner);
    xbox_XcHMAC(hd, 16, inner, 20, NULL, 0, d + n - 20);
}

/* ── the public calls ──────────────────────────────────────────────── */

static int prepare_source(const char *input, const char *title_id, const char *work, char *root, size_t root_n,
                          char *err, unsigned err_size)
{
    char unz[SI_PATH];
    (void)title_id;
    if (!input || !*input) {
        si_err(err, err_size, "No path given.%s", "");
        return 0;
    }
    if (recomp_fs_is_dir(input)) {
        snprintf(root, root_n, "%s", input);
        return 1;
    }
    if (!file_exists(input)) {
        si_err(err, err_size, "%s does not exist.", input);
        return 0;
    }
    if (!ends_with_ci(input, ".zip")) {
        si_err(err, err_size, "%s is not a folder or a .zip file.", input);
        return 0;
    }
    snprintf(unz, sizeof unz, "%s/unzip", work);
    recomp_fs_rm_tree(unz);
    recomp_fs_mkdir_p(unz);
    if (!unzip_to(input, unz, err, err_size))
        return 0;
    snprintf(root, root_n, "%s", unz);
    return 1;
}

int xbox_SaveImportCheckBounded(const char *path, const char *title_id, const xbox_save_check_limits *lim)
{
    char save[SI_PATH];
    int have;
    Lim b;
    if (!path || !*path || !recomp_fs_is_dir(path))
        return 0;          /* a zip is only looked into when it is imported */
    memset(&b, 0, sizeof b);
    b.l = lim;
    b.entries = lim && lim->max_entries > 0 ? lim->max_entries : SI_CHECK_ENTRIES;
    b.deadline = si_now_ms() + (uint64_t)(lim && lim->max_ms > 0 ? lim->max_ms : SI_CHECK_MS);
    if (locate(path, title_id, save, sizeof save, &have, NULL, 0, &b))
        return 1;
    return b.out || b.skipped ? -1 : 0;
}

int xbox_SaveImportCheck(const char *path, const char *title_id)
{
    return xbox_SaveImportCheckBounded(path, title_id, NULL) > 0;
}

void xbox_SaveImportCleanup(const char *work_root)
{
    char w[SI_PATH];
    snprintf(w, sizeof w, "%s/" SI_WORK, work_root);
    recomp_fs_rm_tree(w);
}

int xbox_SaveImportPrepare(const char *input, const char *title_id, const char *keep_root,
                           const char *work_root, char *out, unsigned out_size,
                           char *info, unsigned info_size, char *err, unsigned err_size)
{
    char work[SI_PATH], root[SI_PATH], save[SI_PATH], parent[SI_PATH], id[SI_PATH];
    char dst[SI_PATH], dsave[SI_PATH], p[SI_PATH], q[SI_PATH], name[40], extra[8] = "";
    uint8_t zero[16] = { 0 }, hd[16];
    uint8_t *prof;
    size_t pn = 0;
    int have_tid, nfiles = 0, resign = 1, i;
    FILE *f;

    if (out && out_size)
        out[0] = 0;
    if (info && info_size)
        info[0] = 0;
    if (err && err_size)
        err[0] = 0;
    snprintf(work, sizeof work, "%s/" SI_WORK, work_root);
    recomp_fs_rm_tree(work);
    recomp_fs_mkdir_p(work);
    if (!recomp_fs_is_dir(work)) {
        si_err(err, err_size, "Nothing can be written in %s.", work_root);
        return 0;
    }
    if (!prepare_source(input, title_id, work, root, sizeof root, err, err_size))
        goto fail;
    if (!locate(root, title_id, save, sizeof save, &have_tid, err, err_size, NULL))
        goto fail;
    parent_copy(save, parent, sizeof parent);
    leaf_copy(save, id, sizeof id);
    {
        /* A bare save folder from a zip has no name of its own: the id the
         * title gives its one save. */
        size_t k, n = strlen(id);
        int hex = n >= 8;
        for (k = 0; k < n; k++)
            if (!isxdigit((unsigned char)id[k]))
                hex = 0;
        if (!hex)
            snprintf(id, sizeof id, "57BD267AFF58");
    }

    /* The profile: read, checked, signed again. */
    if (!find_named(save, k_profile, 0, p, sizeof p) || !(prof = read_file(p, &pn, 4u << 20))) {
        si_err(err, err_size, "The profile (\"Profile 1\") in %s could not be read.", save);
        goto fail;
    }
    if (pn < 4096) {
        free(prof);
        si_err(err, err_size, "\"Profile 1\" in %s is only %u bytes: not a Burnout 3 profile.", save, (unsigned)pn);
        goto fail;
    }
    profile_name(prof, pn, name, sizeof name);
    if (xbox_ConsoleKeyLoad(hd)) {
        /* A console key is set (Advanced): the profile is signed for that
         * console, as it came. */
        resign = 0;
    } else {
        sign_profile(prof, pn, zero, zero);
    }

    snprintf(dst, sizeof dst, "%s/out/UDATA/%s", work, title_id);
    snprintf(dsave, sizeof dsave, "%s/%s", dst, id);
    recomp_fs_mkdir_p(dsave);
    snprintf(q, sizeof q, "%s/%s", dsave, k_profile);
    if (!write_file(q, prof, pn)) {
        free(prof);
        si_err(err, err_size, "The save could not be written under %s.", work_root);
        goto fail;
    }
    free(prof);
    nfiles++;

    /* SaveMeta.xbx and SaveImage.xbx as they are; neither is signed. Other
     * files in the folder are not part of a Burnout 3 save and are left. */
    for (i = 0; i < 2; i++) {
        static const char *const want[2] = { "SaveMeta.xbx", "SaveImage.xbx" };
        size_t n2 = 0;
        uint8_t *d;
        if (!find_named(save, want[i], 0, p, sizeof p) || !(d = read_file(p, &n2, 4u << 20)))
            continue;
        snprintf(q, sizeof q, "%s/%s", dsave, want[i]);
        if (write_file(q, d, n2))
            nfiles++;
        free(d);
    }
    if (!find_named(save, k_meta, 0, p, sizeof p)) {
        si_err(err, err_size, "SaveMeta.xbx is missing from %s.", save);
        goto fail;
    }

    /* The title's own files one level up, else the ones in use here. */
    for (i = 0; i < 3; i++) {
        size_t n2 = 0;
        uint8_t *d = NULL;
        char src[SI_PATH];
        if (have_tid && find_named(parent, k_title_files[i], 0, src, sizeof src))
            d = read_file(src, &n2, 4u << 20);
        if (!d && keep_root) {
            char kd[SI_PATH], ku[SI_PATH];
            if (find_named(keep_root, "UDATA", 1, ku, sizeof ku) && find_named(ku, title_id, 1, kd, sizeof kd) &&
                find_named(kd, k_title_files[i], 0, src, sizeof src))
                d = read_file(src, &n2, 4u << 20);
        }
        if (!d)
            continue;
        snprintf(q, sizeof q, "%s/%s", dst, k_title_files[i]);
        if (write_file(q, d, n2))
            nfiles++;
        free(d);
    }

    /* TDATA/<title id> beside UDATA, when the input had the drive's layout. */
    if (have_tid) {
        char ud[SI_PATH], drive[SI_PATH], td[SI_PATH], tt[SI_PATH];
        parent_copy(parent, ud, sizeof ud);
        parent_copy(ud, drive, sizeof drive);
        if (ci_eq(leaf_of(ud), "UDATA") && find_named(drive, "TDATA", 1, td, sizeof td) &&
            find_named(td, title_id, 1, tt, sizeof tt)) {
            snprintf(q, sizeof q, "%s/out/TDATA/%s", work, title_id);
            recomp_fs_copy_tree(tt, q);
        }
    }

    snprintf(p, sizeof p, "%s/out/save-export.txt", work);
    f = fopen(p, "w");
    if (f) {
        fprintf(f, "title %s\nmade %lld\nImported from an original Xbox save.\n", title_id, (long long)time(NULL));
        fclose(f);
    }
    snprintf(out, out_size, "%s/out", work);
    if (!recomp_save_export_check(out, title_id, err, err_size))
        goto fail;
    if (info && info_size)
        snprintf(info, info_size, "Xbox save %s: profile %s, %d files, %s.%s", id, name[0] ? name : "(name not read)",
                 nfiles,
                 resign ? "signed again for this port" : "signature kept (a console key is set)", extra);
    if (recomp_fs_is_dir(work)) {
        char unz[SI_PATH];
        snprintf(unz, sizeof unz, "%s/unzip", work);
        recomp_fs_rm_tree(unz);
    }
    return 1;

fail:
    recomp_fs_rm_tree(work);
    if (out && out_size)
        out[0] = 0;
    return 0;
}

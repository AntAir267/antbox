/*
 * nv2a_pcache.c - the on-disk pipeline-key list; see nv2a_pcache.h.
 */
#include "nv2a_pcache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define pc_mkdir(p) _mkdir(p)
#else
#include <unistd.h>
#define pc_mkdir(p) mkdir((p), 0755)
#endif

#define MAGIC "XRPCACHE"           /* 8 bytes */
#define FORMAT 1u
#define VERSION_MAX 256u

struct rec {
    uint32_t type, len;
    uint64_t sum;
    const unsigned char *data;      /* into pc->buf */
};

struct nv2a_pcache {
    FILE *f;                        /* open for append */
    unsigned char *buf;             /* the file as loaded */
    struct rec *recs;
    size_t nrecs;
    /* Sums of every record in the file, loaded and put: open addressing,
     * 0 is empty (a sum of 0 is stored as 1). */
    uint64_t *set;
    size_t set_cap, set_n;
};

uint64_t nv2a_fnv64(const void *p, size_t n, uint64_t h)
{
    const unsigned char *b = (const unsigned char *)p;
    while (n--) {
        h ^= *b++;
        h *= 0x100000001b3ull;
    }
    return h;
}

static uint64_t rec_sum(uint32_t type, const void *data, uint32_t len)
{
    uint64_t h = nv2a_fnv64(&type, 4, NV2A_FNV64_INIT);
    h = nv2a_fnv64(&len, 4, h);
    h = nv2a_fnv64(data, len, h);
    return h ? h : 1;
}

/* 1 if added, 0 if present, -1 out of memory. */
static int set_add(nv2a_pcache *pc, uint64_t h)
{
    size_t i;
    if ((pc->set_n + 1) * 2 > pc->set_cap) {
        size_t cap = pc->set_cap ? pc->set_cap * 2 : 256, j;
        uint64_t *s = (uint64_t *)calloc(cap, sizeof *s);
        if (!s)
            return -1;
        for (j = 0; j < pc->set_cap; j++) {
            uint64_t v = pc->set[j];
            if (!v)
                continue;
            for (i = (size_t)(v % cap); s[i]; i = (i + 1) % cap)
                ;
            s[i] = v;
        }
        free(pc->set);
        pc->set = s;
        pc->set_cap = cap;
    }
    for (i = (size_t)(h % pc->set_cap); pc->set[i]; i = (i + 1) % pc->set_cap)
        if (pc->set[i] == h)
            return 0;
    pc->set[i] = h;
    pc->set_n++;
    return 1;
}

static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* Header: MAGIC, u32 FORMAT, u32 n, version[n] (no NUL). */
static size_t header(unsigned char *out, const char *version)
{
    size_t n = strlen(version);
    if (n > VERSION_MAX)
        n = VERSION_MAX;
    memcpy(out, MAGIC, 8);
    put32(out + 8, FORMAT);
    put32(out + 12, (uint32_t)n);
    memcpy(out + 16, version, n);
    return 16 + n;
}

static unsigned char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b = NULL;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        b = (unsigned char *)malloc((size_t)n + 1);
        if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
            free(b);
            b = NULL;
        }
        *size = (size_t)n;
    }
    fclose(f);
    return b;
}

/* Writes `len` bytes as the whole file (through a temporary and a rename). */
static int rewrite(const char *path, const unsigned char *b, size_t len)
{
    char tmp[1024];
    FILE *f;
    int ok;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return 0;
    if (!(f = fopen(tmp, "wb")))
        return 0;
    ok = fwrite(b, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
#if defined(_WIN32)
    remove(path);
#endif
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

/* The header length of file image `b` when it reads as `version`, else 0.
 * A version of ours followed by "; toolkit <rev>" is ours too: the key lists
 * up to BO-93 named the toolkit commit, so every rebuild dropped them; the
 * keys under that suffix are the same. */
static size_t header_len(const unsigned char *b, size_t size, const char *version)
{
    static const char old[] = "; toolkit ";
    size_t vl = strlen(version), n;
    if (vl > VERSION_MAX)
        vl = VERSION_MAX;
    if (size < 16 || memcmp(b, MAGIC, 8) != 0 || get32(b + 8) != FORMAT)
        return 0;
    n = get32(b + 12);
    if (n > VERSION_MAX || size - 16 < n || n < vl || memcmp(b + 16, version, vl) != 0)
        return 0;
    if (n > vl && (n - vl < sizeof old - 1 || memcmp(b + 16 + vl, old, sizeof old - 1) != 0))
        return 0;
    return 16 + n;
}

nv2a_pcache *nv2a_pcache_open(const char *path, const char *version,
                              char *msg, size_t msg_len)
{
    unsigned char hdr[16 + VERSION_MAX];
    size_t hlen = header(hdr, version), size = 0, off, cap = 0;
    nv2a_pcache *pc = (nv2a_pcache *)calloc(1, sizeof *pc);
    unsigned char *b;
    const char *what = "new";
    char cut[96] = "";

    if (!pc)
        return NULL;
    b = slurp(path, &size);
    if (b && !(hlen = header_len(b, size, version))) {
        /* Another format, version or no header at all: start afresh. */
        what = size >= 16 && !memcmp(b, MAGIC, 8) ? "another version, started afresh"
                                                   : "not a cache file, started afresh";
        free(b);
        b = NULL;
        remove(path);
        hlen = header(hdr, version);
    }
    if (!b) {
        if (!rewrite(path, hdr, hlen)) {
            if (msg && msg_len)
                snprintf(msg, msg_len, "cannot create %s (%s)", path, strerror(errno));
            free(pc);
            return NULL;
        }
    } else {
        for (off = hlen; off < size; ) {
            uint32_t type, len;
            uint64_t sum;
            if (size - off < 16)
                break;
            type = get32(b + off);
            len = get32(b + off + 4);
            sum = get64(b + off + 8);
            if (len > NV2A_PCACHE_MAX_LEN || size - off - 16 < len
                    || rec_sum(type, b + off + 16, len) != sum)
                break;
            if (pc->nrecs == cap) {
                struct rec *r;
                cap = cap ? cap * 2 : 64;
                if (!(r = (struct rec *)realloc(pc->recs, cap * sizeof *r)))
                    break;
                pc->recs = r;
            }
            pc->recs[pc->nrecs].type = type;
            pc->recs[pc->nrecs].len = len;
            pc->recs[pc->nrecs].sum = sum;
            pc->recs[pc->nrecs].data = b + off + 16;
            if (set_add(pc, sum) > 0)
                pc->nrecs++;
            off += 16 + (size_t)len;
        }
        if (off < size) {
            /* A torn or damaged record: keep what came before it. */
            snprintf(cut, sizeof cut, ", %zu bytes cut at offset %zu", size - off, off);
            rewrite(path, b, off);
        }
        what = "loaded";
        pc->buf = b;
    }
    pc->f = fopen(path, "ab");
    /* A buffer larger than any record, so a record leaves in one write. */
    if (pc->f)
        setvbuf(pc->f, NULL, _IOFBF, 16 + NV2A_PCACHE_MAX_LEN);
    if (!pc->f) {
        if (msg && msg_len)
            snprintf(msg, msg_len, "cannot append to %s (%s)", path, strerror(errno));
        nv2a_pcache_close(pc);
        return NULL;
    }
    if (msg && msg_len)
        snprintf(msg, msg_len, "%s: %zu records%s", what, pc->nrecs, cut);
    return pc;
}

size_t nv2a_pcache_count(const nv2a_pcache *pc)
{
    return pc ? pc->nrecs : 0;
}

int nv2a_pcache_get(const nv2a_pcache *pc, size_t i, uint32_t *type,
                    const void **data, uint32_t *len)
{
    if (!pc || i >= pc->nrecs)
        return 0;
    *type = pc->recs[i].type;
    *data = pc->recs[i].data;
    *len = pc->recs[i].len;
    return 1;
}

int nv2a_pcache_put(nv2a_pcache *pc, uint32_t type, const void *data, uint32_t len)
{
    unsigned char stack[16 + 4096], *r = stack;
    uint64_t sum;
    int added, ok;

    if (!pc || !pc->f || len > NV2A_PCACHE_MAX_LEN)
        return -1;
    sum = rec_sum(type, data, len);
    if ((added = set_add(pc, sum)) <= 0)
        return added;
    if (16 + (size_t)len > sizeof stack && !(r = (unsigned char *)malloc(16 + (size_t)len)))
        return -1;
    put32(r, type);
    put32(r + 4, len);
    put64(r + 8, sum);
    memcpy(r + 16, data, len);
    /* One write of the whole record (O_APPEND under "ab"), so two processes
     * appending at once interleave whole records. */
    ok = fwrite(r, 1, 16 + (size_t)len, pc->f) == 16 + (size_t)len && fflush(pc->f) == 0;
    if (r != stack)
        free(r);
    return ok ? 1 : -1;
}

void nv2a_pcache_close(nv2a_pcache *pc)
{
    if (!pc)
        return;
    if (pc->f)
        fclose(pc->f);
    free(pc->buf);
    free(pc->recs);
    free(pc->set);
    free(pc);
}

/* mkdir -p */
static int make_dirs(char *p)
{
    char *s;
    struct stat st;
    for (s = p + 1; *s; s++) {
        if (*s != '/' && *s != '\\')
            continue;
        *s = 0;
        if (stat(p, &st) != 0 && pc_mkdir(p) != 0 && errno != EEXIST) {
            *s = '/';
            return 0;
        }
        *s = '/';
    }
    return (stat(p, &st) == 0 && (st.st_mode & S_IFDIR)) || pc_mkdir(p) == 0
        || errno == EEXIST;
}

#if defined(__linux__) && !defined(__ANDROID__)
extern char *program_invocation_short_name;
#endif

static char s_app[64];

void nv2a_pcache_set_app(const char *app)
{
    snprintf(s_app, sizeof s_app, "%s", app ? app : "");
}

const char *nv2a_pcache_app(void)
{
    if (s_app[0])
        return s_app;
#if defined(__APPLE__)
    return getprogname();
#elif defined(__linux__) && !defined(__ANDROID__)
    return program_invocation_short_name;
#else
    return "xboxrecomp";
#endif
}

int nv2a_pcache_dir(const char *dir, const char *app, char *out, size_t n)
{
    const char *home = getenv("HOME");
    int w;

    if (dir && (!*dir || !strcmp(dir, "none") || !strcmp(dir, "off") || !strcmp(dir, "0")))
        return 0;
    if (!app || !*app)
        app = "xboxrecomp";
    if (dir) {
        if (dir[0] == '~' && (dir[1] == '/' || dir[1] == '\\' || !dir[1]) && home)
            w = snprintf(out, n, "%s%s", home, dir + 1);
        else
            w = snprintf(out, n, "%s", dir);
    } else {
#if defined(__APPLE__)
        if (!home)
            return 0;
        w = snprintf(out, n, "%s/Library/Caches/%s", home, app);
#elif defined(_WIN32)
        const char *la = getenv("LOCALAPPDATA");
        if (!la)
            return 0;
        w = snprintf(out, n, "%s\\%s\\cache", la, app);
#else
        const char *xdg = getenv("XDG_CACHE_HOME");
        if (xdg && *xdg)
            w = snprintf(out, n, "%s/%s", xdg, app);
        else if (home)
            w = snprintf(out, n, "%s/.cache/%s", home, app);
        else
            return 0;
#endif
    }
    if (w <= 0 || (size_t)w >= n)
        return 0;
    return make_dirs(out);
}

/* ---- the running backend's numbers ------------------------------------------- */

static nv2a_pcache_stat s_stat;
static void (*s_rescan)(void);

void nv2a_pcache_stat_get(nv2a_pcache_stat *out)
{
    *out = s_stat;
}

void nv2a_pcache_stat_open(const char *dir, const uint32_t listed[3])
{
    snprintf(s_stat.dir, sizeof s_stat.dir, "%s", dir ? dir : "");
    memcpy(s_stat.listed, listed, sizeof s_stat.listed);
    s_stat.active = dir && *dir;
}

void nv2a_pcache_stat_warming(int on)
{
    s_stat.warming = on;
    if (!on)
        s_stat.hdr_round = 0;
}

void nv2a_pcache_stat_hdr(int on)
{
    s_stat.hdr_round = on;
}

void nv2a_pcache_stat_waiting(int on)
{
    s_stat.waiting = on;
}

void nv2a_pcache_stat_warm_ms(uint32_t ms)
{
    s_stat.warm_ms = ms ? ms : 1;
}

void nv2a_pcache_stat_hold(uint32_t done, uint32_t total, int cold)
{
    s_stat.hold_done = done;
    s_stat.hold_total = total;
    s_stat.cold = cold;
}

void nv2a_pcache_stat_built(int prewarm, int kind, uint64_t ns)
{
    if (kind < 0 || kind > 2)
        return;
    if (prewarm) {
        s_stat.warmed[kind]++;
    } else {
        uint64_t us = ns / 1000u;
        s_stat.ack[kind]++;
        s_stat.ack_ns += ns;
        if (us > s_stat.ack_max_us)
            s_stat.ack_max_us = us > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)us;
    }
}

static void (*s_seed)(uint32_t, const void *, uint32_t);

void nv2a_pcache_set_seed(void (*fn)(uint32_t type, const void *data, uint32_t len))
{
    s_seed = fn;
}

int nv2a_pcache_seed(uint32_t type, const void *data, uint32_t len)
{
    if (!s_seed || !s_stat.active)
        return 0;
    s_seed(type, data, len);
    return 1;
}

void nv2a_pcache_set_rescan(void (*fn)(void))
{
    s_rescan = fn;
}

int nv2a_pcache_rescan(void)
{
    if (!s_rescan || !s_stat.active)
        return 0;
    s_rescan();
    return 1;
}

/* ---- vertex programs in an executable ------------------------------------------ */

size_t nv2a_pcache_scan_vsh(const void *image, size_t size,
                            void (*fn)(void *ctx, const uint32_t *slots, uint32_t n),
                            void *ctx)
{
    const unsigned char *b = (const unsigned char *)image;
    uint32_t slots[136 * 4];
    size_t i, found = 0;

    for (i = 0; i + 4 + 16 <= size; i++) {
        uint32_t n, k;
        if (b[i] != 0x78 || b[i + 1] != 0x20)
            continue;
        n = (uint32_t)b[i + 2] | ((uint32_t)b[i + 3] << 8);
        if (!n || n > 136 || size - i - 4 < (size_t)n * 16u)
            continue;
        memcpy(slots, b + i + 4, (size_t)n * 16u);
        for (k = 0; k < n; k++)
            if (slots[k * 4] != 0 || ((slots[k * 4 + 3] & 1u) != 0) != (k == n - 1))
                break;
        if (k < n)
            continue;
        if (fn)
            fn(ctx, slots, n);
        found++;
        i += 3 + (size_t)n * 16u;
    }
    return found;
}

/* ---- the shipped manifest ------------------------------------------------------- */

static char s_man_xbe[1100];
static struct { char backend[16]; unsigned char *data; size_t size; } s_man[4];

void nv2a_pcache_set_manifest(const char *backend, const void *data, size_t size)
{
    int i;
    for (i = 0; i < 4 && s_man[i].data && strcmp(s_man[i].backend, backend); i++)
        ;
    if (i == 4 || !(s_man[i].data = (unsigned char *)realloc(s_man[i].data, size ? size : 1)))
        return;
    memcpy(s_man[i].data, data, size);
    s_man[i].size = size;
    snprintf(s_man[i].backend, sizeof s_man[i].backend, "%s", backend);
}

void nv2a_pcache_set_xbe(const char *xbe_path)
{
    snprintf(s_man_xbe, sizeof s_man_xbe, "%s", xbe_path ? xbe_path : "");
}

struct man_vs { uint64_t hash; uint32_t n; uint32_t *slots; };
struct man_scan { struct man_vs *v; size_t n, cap; };

static void man_vs_add(void *ctx, const uint32_t *slots, uint32_t n)
{
    struct man_scan *s = (struct man_scan *)ctx;
    if (s->n == s->cap) {
        struct man_vs *v = (struct man_vs *)realloc(s->v, (s->cap ? s->cap * 2 : 64) * sizeof *v);
        if (!v)
            return;
        s->v = v;
        s->cap = s->cap ? s->cap * 2 : 64;
    }
    s->v[s->n].slots = (uint32_t *)malloc((size_t)n * 16u);
    if (!s->v[s->n].slots)
        return;
    memcpy(s->v[s->n].slots, slots, (size_t)n * 16u);
    s->v[s->n].n = n;
    s->v[s->n].hash = nv2a_fnv64(slots, (size_t)n * 16u, NV2A_FNV64_INIT);
    s->n++;
}


static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* Programs the executable holds as bare runs of slots, without the 0x2078
 * header the XDK assembler writes (the ones D3D's own code uploads, such as the
 * fixed-function program the title draws its overlays with): a run of 1..136
 * slots whose first word is 0, whose later words are not all 0, and of which only
 * the last has FINAL (bit 0 of the fourth word). Only those whose hash the
 * manifest lists (`man`, its records from `from`) and the header scan has not
 * found are kept. */
static void man_scan_raw(const unsigned char *man, size_t man_size, size_t from,
                         const unsigned char *image, size_t size, struct man_scan *scan)
{
    uint64_t *want = NULL, *seen = NULL;
    size_t nw = 0, cap = 0, off, i;

    for (off = from; off + 16 <= man_size; ) {
        uint32_t type = get32(man + off), len = get32(man + off + 4);
        if (len > NV2A_PCACHE_MAX_LEN || man_size - off - 16 < len)
            break;
        if (type == NV2A_PC_MANIFEST_VS && len == 8) {
            uint64_t h = get64(man + off + 16);
            int have = 0;
            for (i = 0; i < scan->n && !have; i++)
                have = scan->v[i].hash == h;
            if (!have) {
                if (nw == cap) {
                    uint64_t *w = (uint64_t *)realloc(want, (cap ? cap * 2 : 32) * sizeof *w);
                    if (!w)
                        break;
                    want = w;
                    cap = cap ? cap * 2 : 32;
                }
                want[nw++] = h;
            }
        }
        off += 16 + (size_t)len;
    }
    if (nw) {
        qsort(want, nw, sizeof *want, cmp_u64);
        seen = (uint64_t *)calloc(nw, sizeof *seen);
        for (off = 0; seen && off + 16 <= size; off += 4) {
            const unsigned char *p = image + off;
            uint32_t n = 0;
            for (;;) {
                const unsigned char *s = p + (size_t)n * 16u;
                uint32_t w1, w2, w3;
                if ((size_t)(s - image) + 16 > size || get32(s) != 0)
                    break;
                w1 = get32(s + 4); w2 = get32(s + 8); w3 = get32(s + 12);
                if (!(w1 | w2 | w3))
                    break;
                n++;
                if (w3 & 1u)
                    break;
                if (n >= 136) {
                    n = 0;
                    break;
                }
            }
            if (n && (get32(p + (size_t)(n - 1) * 16u + 12) & 1u)) {
                uint64_t h = nv2a_fnv64(p, (size_t)n * 16u, NV2A_FNV64_INIT);
                uint64_t *hit = (uint64_t *)bsearch(&h, want, nw, sizeof *want, cmp_u64);
                if (hit && !seen[hit - want]) {
                    seen[hit - want] = 1;
                    man_vs_add(scan, (const uint32_t *)(const void *)p, n);   /* memcpy'd there */
                }
            }
        }
    }
    free(seen);
    free(want);
}

uint32_t nv2a_pcache_manifest_apply(nv2a_pcache *dst, const char *backend,
                                    const char *version, nv2a_pcache_manifest_stat *st)
{
    nv2a_pcache_manifest_stat z;
    unsigned char *b, *xbe = NULL;
    size_t size = 0, xsize = 0, off, vl, n, i;
    struct man_scan scan;
    static const char tag[] = "; manifest ";

    if (!st)
        st = &z;
    memset(st, 0, sizeof *st);
    memset(&scan, 0, sizeof scan);
    const char *over = getenv("RECOMP_PCACHE_MANIFEST"), *label = "(built in)";
    int mi;

    if (!dst || (over && !strcmp(over, "none")))
        return 0;
    if (over && *over) {
        label = over;
        b = slurp(over, &size);
    } else {
        for (mi = 0; mi < 4 && (!s_man[mi].data || strcmp(s_man[mi].backend, backend)); mi++)
            ;
        if (mi == 4 || !s_man[mi].size)
            return 0;
        size = s_man[mi].size;
        b = (unsigned char *)malloc(size);
        if (b)
            memcpy(b, s_man[mi].data, size);
    }
    if (!b)
        return 0;
    vl = strlen(version);
    n = size >= 16 ? get32(b + 12) : 0;
    if (size < 16 || memcmp(b, MAGIC, 8) != 0 || get32(b + 8) != FORMAT || n > VERSION_MAX
            || size - 16 < n || n < vl + sizeof tag - 1 || memcmp(b + 16, version, vl) != 0
            || memcmp(b + 16 + vl, tag, sizeof tag - 1) != 0) {
        fprintf(stderr, "[pcache] manifest %s is of another key format: ignored\n", label);
        free(b);
        return 0;
    }
    st->found = 1;
    {
        size_t rl = n - vl - (sizeof tag - 1);
        if (rl >= sizeof st->revision)
            rl = sizeof st->revision - 1;
        memcpy(st->revision, b + 16 + vl + sizeof tag - 1, rl);
        st->revision[rl] = 0;
    }
    if (s_man_xbe[0] && (xbe = slurp(s_man_xbe, &xsize)) != NULL) {
        nv2a_pcache_scan_vsh(xbe, xsize, man_vs_add, &scan);
        man_scan_raw(b, size, 16 + n, xbe, xsize, &scan);
    }
    for (off = 16 + n; off < size; ) {
        uint32_t type, len;
        uint64_t sum;
        if (size - off < 16)
            break;
        type = get32(b + off);
        len = get32(b + off + 4);
        sum = get64(b + off + 8);
        if (len > NV2A_PCACHE_MAX_LEN || size - off - 16 < len
                || rec_sum(type, b + off + 16, len) != sum)
            break;
        st->records++;
        if (type == NV2A_PC_MANIFEST_VS && len == 8) {
            uint64_t h = get64(b + off + 16);
            st->vs_listed++;
            for (i = 0; i < scan.n; i++)
                if (scan.v[i].hash == h) {
                    st->vs_resolved++;
                    if (nv2a_pcache_put(dst, 0, scan.v[i].slots, scan.v[i].n * 16u) > 0)
                        st->added++;
                    break;
                }
        } else if (type != NV2A_PC_MANIFEST_VS) {
            if (nv2a_pcache_put(dst, type, b + off + 16, len) > 0)
                st->added++;
        }
        off += 16 + (size_t)len;
    }
    for (i = 0; i < scan.n; i++)
        free(scan.v[i].slots);
    free(scan.v);
    free(xbe);
    free(b);
    fprintf(stderr, "[pcache] manifest %s rev %s: %u records, %u of %u vertex programs found in"
            " the executable, %u new to the key list\n", label, st->revision, st->records,
            st->vs_resolved, st->vs_listed, st->added);
    return st->added;
}

#if defined(__linux__) || defined(__ANDROID__)
#include <sys/resource.h>
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

void nv2a_pcache_lower_priority(void)
{
#if defined(__linux__) || defined(__ANDROID__)
    (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#elif defined(__APPLE__)
    (void)pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

int nv2a_pcache_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? (int)si.dwNumberOfProcessors : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/*
 * nv2a_texpack.c - texture packs: the key index, the jobs and their worker
 * threads, the budget numbers, the disk cache and the dump; see
 * nv2a_texpack.h. The pixels are in nv2a_texpack_img.c.
 */
#include "nv2a_texpack.h"
#include "nv2a_backend_common.h"
#include "nv2a_texrep.h"
#include "recomp_env.h"
#include "host_time.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#define LOGP "[TEXPACK] "

/* ── names and keys ───────────────────────────────────────────────────── */

static int hex16(const char *s, uint64_t *v)
{
    uint64_t r = 0;
    int i;
    for (i = 0; i < 16; i++) {
        int c = (unsigned char)s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return 0;
        r = r << 4 | (uint64_t)d;
    }
    *v = r;
    return 1;
}

int nv2a_texpack_parse_name(const char *name, uint64_t *hash, uint64_t *palh)
{
    size_t n = name ? strlen(name) : 0;
    uint64_t h, p = 0;
    if (n != 16 + 4 && n != 16 + 1 + 16 + 4)
        return 0;
    if (tolower((unsigned char)name[n - 4]) != '.' || tolower((unsigned char)name[n - 3]) != 'p'
            || tolower((unsigned char)name[n - 2]) != 'n' || tolower((unsigned char)name[n - 1]) != 'g')
        return 0;
    if (!hex16(name, &h))
        return 0;
    if (n > 20) {
        if (name[16] != '-' || !hex16(name + 17, &p) || !p)
            return 0;
    }
    *hash = h;
    *palh = p;
    return 1;
}

void nv2a_texpack_key_name(uint64_t hash, uint64_t palh, char *out, size_t n)
{
    if (palh)
        snprintf(out, n, "%016llx-%016llx.png", (unsigned long long)hash, (unsigned long long)palh);
    else
        snprintf(out, n, "%016llx.png", (unsigned long long)hash);
}

int nv2a_texpack_key(const uint8_t *guest, uint32_t color, uint32_t w, uint32_t h,
                     uint32_t pitch, const uint8_t *pal, uint32_t pal_len,
                     uint64_t *hash, uint64_t *palh)
{
    *hash = nv2a_texrep_hash(guest, color, w, h, pitch);
    *palh = 0;
    if (!*hash)
        return 0;
    if (color == NV2A_TEX_P8 && pal && pal_len)
        *palh = nv2a_tex_hash(pal, pal_len * 4u, NV2A_TEX_HASH_SEED);
    return 1;
}

/* ── settings ─────────────────────────────────────────────────────────── */

static int handheld(void)
{
#if defined(__ANDROID__)
    return 1;
#else
    return recomp_host_is_steam_deck();
#endif
}

static double env_num(int id_present, const char *e, double def)
{
    (void)id_present;
    return e && *e && atof(e) >= 0 ? atof(e) : def;
}

uint64_t nv2a_texpack_cap_bytes(void)
{
    static uint64_t cap;
    if (!cap) {
        const char *e = recomp_env(RENV_TEXPACK_CAP_MB);
        double mb = e && atof(e) > 0 ? atof(e) : handheld() ? 256.0 : 1024.0;
        cap = (uint64_t)(mb * 1048576.0);
    }
    return cap;
}

size_t nv2a_texpack_swap_bytes(void)
{
    static size_t n;
    if (!n) {
        const char *e = recomp_env(RENV_TEXPACK_SWAP_MB);
        double mb = e && atof(e) > 0 ? atof(e) : 24.0;
        n = (size_t)(mb * 1048576.0);
    }
    return n;
}

int nv2a_texpack_swap_fits(size_t bytes, size_t left)
{
    return bytes <= left || left == nv2a_texpack_swap_bytes();
}

static atomic_uint s_formats;

void nv2a_texpack_set_formats(unsigned mask)
{
    atomic_store(&s_formats, mask);
}

int nv2a_texpack_compress_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = recomp_env(RENV_TEXPACK_COMPRESS);
        on = !(e && *e == '0');
    }
    return on;
}

int nv2a_texpack_log(void)
{
    static int on = -1;
    if (on < 0)
        on = recomp_env(RENV_TEXPACK_LOG) != NULL;
    return on;
}

/* ── the index ────────────────────────────────────────────────────────── */

typedef struct {
    uint64_t hash, palh;
    uint32_t pack;
    uint32_t name;      /* offset in s_names */
} Entry;

typedef struct {
    char *dir, *name;
    int   alpha, compress;
} Pack;

static pthread_mutex_t s_ix = PTHREAD_MUTEX_INITIALIZER;
static Pack    s_pack[NV2A_TEXPACK_MAX_PACKS];
static unsigned s_npack;
static Entry  *s_ent;
static size_t  s_nent;
static char   *s_names;
static size_t  s_names_n, s_names_cap;
static atomic_uint s_gen = 1;
static atomic_int  s_active;
static atomic_int  s_set_called;

static struct nv2a_texpack_stats s_st;

static int entry_cmp(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->hash != y->hash)
        return x->hash < y->hash ? -1 : 1;
    if (x->palh != y->palh)
        return x->palh < y->palh ? -1 : 1;
    return x->pack < y->pack ? -1 : x->pack > y->pack;
}

static void packs_free(void)
{
    unsigned i;
    for (i = 0; i < s_npack; i++) {
        free(s_pack[i].dir);
        free(s_pack[i].name);
    }
    s_npack = 0;
    free(s_ent);
    s_ent = NULL;
    s_nent = 0;
    free(s_names);
    s_names = NULL;
    s_names_n = s_names_cap = 0;
}

static uint32_t name_add(const char *name)
{
    size_t n = strlen(name) + 1;
    if (s_names_n + n > s_names_cap) {
        size_t cap = s_names_cap ? s_names_cap * 2 : 65536;
        char *p;
        while (cap < s_names_n + n)
            cap *= 2;
        p = (char *)realloc(s_names, cap);
        if (!p)
            return 0xFFFFFFFFu;
        s_names = p;
        s_names_cap = cap;
    }
    memcpy(s_names + s_names_n, name, n);
    s_names_n += n;
    return (uint32_t)(s_names_n - n);
}

/* One pack folder's PNG names into a list of entries (s_ent grows). */
static size_t s_ent_cap;

static int list_pack(unsigned pi)
{
    DIR *d = opendir(s_pack[pi].dir);
    struct dirent *de;
    int n = 0;

    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        uint64_t h, p;
        uint32_t off;
        if (!nv2a_texpack_parse_name(de->d_name, &h, &p))
            continue;
        if (s_nent == s_ent_cap) {
            size_t cap = s_ent_cap ? s_ent_cap * 2 : 1024;
            Entry *e = (Entry *)realloc(s_ent, cap * sizeof *e);
            if (!e)
                break;
            s_ent = e;
            s_ent_cap = cap;
        }
        off = name_add(de->d_name);
        if (off == 0xFFFFFFFFu)
            break;
        s_ent[s_nent].hash = h;
        s_ent[s_nent].palh = p;
        s_ent[s_nent].pack = pi;
        s_ent[s_nent].name = off;
        s_nent++;
        n++;
    }
    closedir(d);
    return n;
}

int nv2a_texpack_set(const struct nv2a_texpack_src *packs, unsigned n)
{
    struct nv2a_texpack_src all[NV2A_TEXPACK_MAX_PACKS];
    unsigned na = 0, i;
    const char *env = recomp_env(RENV_TEXPACK);
    size_t out = 0, in;
    static char envdirs[2048];

    /* RECOMP_DEBUG=texpack=<dir>[;<dir>] first. */
    if (env && *env) {
        char *p, *q;
        snprintf(envdirs, sizeof envdirs, "%s", env);
        for (p = envdirs; p && *p && na < NV2A_TEXPACK_MAX_PACKS;) {
            q = strchr(p, ';');
            if (q)
                *q++ = 0;
            if (*p) {
                all[na].dir = p;
                all[na].name = "texpack";
                all[na].alpha = NV2A_TEXPACK_ALPHA_AUTO;
                all[na].compress = NV2A_TEXPACK_COMPRESS_AUTO;
                na++;
            }
            p = q;
        }
    }
    for (i = 0; packs && i < n && na < NV2A_TEXPACK_MAX_PACKS; i++)
        if (packs[i].dir && *packs[i].dir)
            all[na++] = packs[i];

    atomic_store(&s_set_called, 1);
    pthread_mutex_lock(&s_ix);
    packs_free();
    s_ent_cap = 0;
    for (i = 0; i < na; i++) {
        int k;
        s_pack[i].dir = strdup(all[i].dir);
        s_pack[i].name = strdup(all[i].name ? all[i].name : "pack");
        s_pack[i].alpha = all[i].alpha;
        s_pack[i].compress = all[i].compress;
        s_npack = i + 1;
        k = list_pack(i);
        if (k < 0)
            fprintf(stderr, LOGP "pack %s: cannot read %s\n", s_pack[i].name, s_pack[i].dir);
        else
            fprintf(stderr, LOGP "pack %s: %d textures in %s\n", s_pack[i].name, k, s_pack[i].dir);
    }
    if (s_nent)
        qsort(s_ent, s_nent, sizeof *s_ent, entry_cmp);
    /* One entry a key: the first pack (the sort put it first). */
    for (in = 0; in < s_nent; in++) {
        if (out && s_ent[out - 1].hash == s_ent[in].hash && s_ent[out - 1].palh == s_ent[in].palh)
            continue;
        s_ent[out++] = s_ent[in];
    }
    s_nent = out;
    s_st.keys = s_nent;
    s_st.packs = s_npack;
    atomic_store(&s_active, s_nent != 0);
    if (atomic_fetch_add(&s_gen, 1) == 0xFFFFFFFFu)
        atomic_store(&s_gen, 1);
    pthread_mutex_unlock(&s_ix);
    if (s_nent)
        fprintf(stderr, LOGP "%zu texture keys in %u pack%s\n", s_nent, s_npack,
                s_npack == 1 ? "" : "s");
    return (int)s_nent;
}

uint32_t nv2a_texpack_generation(void)
{
    return atomic_load(&s_gen);
}

int nv2a_texpack_active(void)
{
    static atomic_int env_done;
    /* A title that never calls nv2a_texpack_set still gets RECOMP_DEBUG=texpack. */
    if (!atomic_load(&env_done) && !atomic_exchange(&env_done, 1) && recomp_env(RENV_TEXPACK)
            && !atomic_load(&s_set_called))
        nv2a_texpack_set(NULL, 0);
    return atomic_load(&s_active);
}

static const Entry *lookup(uint64_t hash, uint64_t palh)
{
    size_t lo = 0, hi = s_nent;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        const Entry *e = &s_ent[mid];
        int c = e->hash != hash ? (e->hash < hash ? -1 : 1) : e->palh != palh ? (e->palh < palh ? -1 : 1) : 0;
        if (!c)
            return e;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

uint32_t nv2a_texpack_find(uint64_t hash, uint64_t palh)
{
    const Entry *e;
    uint32_t id = 0;
    if (!atomic_load(&s_active))
        return 0;
    pthread_mutex_lock(&s_ix);
    e = lookup(hash, palh);
    if (!e && palh)
        e = lookup(hash, 0);
    if (e)
        id = (uint32_t)(e - s_ent) + 1;
    pthread_mutex_unlock(&s_ix);
    return id;
}

int nv2a_texpack_pack_of(uint32_t id)
{
    int r = -1;
    pthread_mutex_lock(&s_ix);
    if (id && id <= s_nent)
        r = (int)s_ent[id - 1].pack;
    pthread_mutex_unlock(&s_ix);
    return r;
}

/* The file of an id, its pack's alpha and compress settings (s_ix not held).
 * gen: the generation the id was found under (0: any). The packs changed
 * since (nv2a_texpack_set from another thread): the id may name another file
 * now, so it names none. */
static int entry_info(uint32_t id, uint32_t gen, char *path, size_t n, int *alpha, int *compress,
                      char *pname, size_t pn)
{
    int ok = 0;
    pthread_mutex_lock(&s_ix);
    if (id && id <= s_nent && (!gen || gen == atomic_load(&s_gen))) {
        const Entry *e = &s_ent[id - 1];
        const Pack *p = &s_pack[e->pack];
        snprintf(path, n, "%s/%s", p->dir, s_names + e->name);
        *alpha = p->alpha;
        *compress = p->compress;
        snprintf(pname, pn, "%s", p->name);
        ok = 1;
    }
    pthread_mutex_unlock(&s_ix);
    return ok;
}

/* ── the disk cache ───────────────────────────────────────────────────── */

#define CACHE_VERSION 1u
#define CACHE_MAGIC 0x314B5054u    /* "TPK1" */

static pthread_mutex_t s_cm = PTHREAD_MUTEX_INITIALIZER;
static char     s_cdir[900];
static uint64_t s_climit = 4096ull << 20;
static int      s_cswept;

struct cache_hdr {
    uint32_t magic, version, w, h, levels, fmt, add, pad;
    uint64_t bytes, key;
};

void nv2a_texpack_cache_dir(const char *dir)
{
    const char *e = recomp_env(RENV_TEXPACK_CACHE_MB);
    pthread_mutex_lock(&s_cm);
    snprintf(s_cdir, sizeof s_cdir, "%s", dir ? dir : "");
    if (e && *e)
        s_climit = (uint64_t)(env_num(1, e, 4096.0) * 1048576.0);
    if (!s_climit)
        s_cdir[0] = 0;
    s_cswept = 0;
    pthread_mutex_unlock(&s_cm);
}

static void mkdirs(const char *dir)
{
    char p[1000];
    size_t i, n = strlen(dir);
    if (n >= sizeof p)
        return;
    memcpy(p, dir, n + 1);
    for (i = 1; i <= n; i++) {
        if (p[i] == '/' || p[i] == '\\' || p[i] == 0) {
            char c = p[i];
            p[i] = 0;
#if defined(_WIN32)
            mkdir(p);
#else
            mkdir(p, 0755);
#endif
            p[i] = c;
        }
    }
}

typedef struct { time_t t; uint64_t size; char name[64]; } CEnt;

static int cent_cmp(const void *a, const void *b)
{
    const CEnt *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t;
}

/* The folder's size; past the bound the oldest files go until three quarters
 * of it are left. A worker, s_cm held. */
static void cache_sweep(void)
{
    DIR *d = opendir(s_cdir);
    struct dirent *de;
    CEnt *v = NULL;
    size_t n = 0, cap = 0, i;
    uint64_t total = 0;
    time_t now = time(NULL);

    s_cswept = 1;
    if (!d)
        return;
    while ((de = readdir(d)) != NULL) {
        char path[1000];
        struct stat st;
        size_t len = strlen(de->d_name);
        int tmp = len > 4 && !strcmp(de->d_name + len - 4, ".tmp");
        if (len >= sizeof v->name || len < 5 || (!tmp && strcmp(de->d_name + len - 4, ".tpk")))
            continue;
        snprintf(path, sizeof path, "%s/%s", s_cdir, de->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (tmp) {
            if (now - st.st_mtime > 600)
                remove(path);
            continue;
        }
        if (n == cap) {
            CEnt *p = (CEnt *)realloc(v, (cap ? cap * 2 : 256) * sizeof *v);
            if (!p)
                break;
            v = p;
            cap = cap ? cap * 2 : 256;
        }
        v[n].t = st.st_mtime;
        v[n].size = (uint64_t)st.st_size;
        memcpy(v[n].name, de->d_name, len + 1);
        total += v[n].size;
        n++;
    }
    closedir(d);
    if (total > s_climit && n) {
        qsort(v, n, sizeof *v, cent_cmp);
        for (i = 0; i < n && total > s_climit / 4 * 3; i++) {
            char path[1000];
            snprintf(path, sizeof path, "%s/%s", s_cdir, v[i].name);
            if (remove(path) == 0)
                total -= v[i].size;
        }
    }
    free(v);
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    return nv2a_tex_hash((const uint8_t *)p, (uint32_t)n, h);
}

static int cache_load(uint64_t key, struct nv2a_texpack_img *img)
{
    char path[1000];
    FILE *f;
    struct cache_hdr hd;
    pthread_mutex_lock(&s_cm);
    if (!s_cdir[0]) {
        pthread_mutex_unlock(&s_cm);
        return 0;
    }
    snprintf(path, sizeof path, "%s/%016llx.tpk", s_cdir, (unsigned long long)key);
    pthread_mutex_unlock(&s_cm);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(&hd, 1, sizeof hd, f) != sizeof hd || hd.magic != CACHE_MAGIC
            || hd.version != CACHE_VERSION || hd.key != key || !hd.bytes
            || hd.bytes > (1ull << 30) || hd.fmt == NV2A_TEXPACK_FMT_BGRA8) {
        fclose(f);
        remove(path);
        return 0;
    }
    img->data = (uint8_t *)malloc((size_t)hd.bytes);
    if (!img->data || fread(img->data, 1, (size_t)hd.bytes, f) != hd.bytes) {
        free(img->data);
        img->data = NULL;
        fclose(f);
        remove(path);
        return 0;
    }
    fclose(f);
    img->w = hd.w;
    img->h = hd.h;
    img->levels = hd.levels;
    img->fmt = hd.fmt;
    img->add = hd.add;
    img->bytes = (size_t)hd.bytes;
    return 1;
}

static void cache_save(uint64_t key, const struct nv2a_texpack_img *img)
{
    char path[1000], tmp[1040];
    struct cache_hdr hd;
    FILE *f;
    int ok;

    pthread_mutex_lock(&s_cm);
    if (!s_cdir[0]) {
        pthread_mutex_unlock(&s_cm);
        return;
    }
    snprintf(path, sizeof path, "%s/%016llx.tpk", s_cdir, (unsigned long long)key);
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
    memset(&hd, 0, sizeof hd);
    hd.magic = CACHE_MAGIC;
    hd.version = CACHE_VERSION;
    hd.w = img->w;
    hd.h = img->h;
    hd.levels = img->levels;
    hd.fmt = img->fmt;
    hd.add = img->add;
    hd.bytes = img->bytes;
    hd.key = key;
    f = fopen(tmp, "wb");
    if (!f) {
        mkdirs(s_cdir);
        f = fopen(tmp, "wb");
    }
    if (!f) {
        pthread_mutex_unlock(&s_cm);
        return;
    }
    ok = fwrite(&hd, 1, sizeof hd, f) == sizeof hd && fwrite(img->data, 1, img->bytes, f) == img->bytes;
    ok = fclose(f) == 0 && ok;
#if defined(_WIN32)
    if (ok)
        remove(path);
#endif
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
    } else {
        s_st.cache_written++;
        if (!s_cswept)
            cache_sweep();
    }
    pthread_mutex_unlock(&s_cm);
}

/* ── the jobs ─────────────────────────────────────────────────────────── */

#define MAX_JOBS 512
#define MAX_WORKERS 2

enum { J_FREE = 0, J_QUEUED, J_RUNNING, J_READY, J_FAILED };

typedef struct {
    int      state;
    uint32_t serial;
    uint32_t id;
    uint32_t refs;
    uint32_t ow, oh, olevels, cut;
    uint32_t gen;             /* the packs' generation the id is of (0: any) */
    uint8_t *alpha;           /* ow*oh, or NULL */
    uint64_t akey;            /* hash of the alpha plane */
    struct nv2a_texpack_img img;
    size_t   cpu;             /* bytes counted against the bound */
    int      from_cache;
} Job;

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  s_idle = PTHREAD_COND_INITIALIZER;
static Job      s_job[MAX_JOBS];
static int      s_queue[MAX_JOBS];
static unsigned s_qh, s_qn;
static unsigned s_busy;
static pthread_t s_thr[MAX_WORKERS];
static int      s_started;
static uint32_t s_serial;
static uint64_t s_cpu_cap;

static uint64_t cpu_cap(void)
{
    if (!s_cpu_cap) {
        const char *e = recomp_env(RENV_TEXPACK_CPU_MB);
        s_cpu_cap = (uint64_t)(env_num(1, e, handheld() ? 128.0 : 512.0) * 1048576.0);
    }
    return s_cpu_cap;
}

static void job_free_locked(Job *j)
{
    s_st.cpu_bytes -= j->cpu;
    free(j->alpha);
    nv2a_texpack_img_free(&j->img);
    memset(j, 0, sizeof *j);
}

/* The cache key of what a job produces. */
static uint64_t job_key(const char *path, const Job *j, int alpha_mode, uint32_t fmt_pref)
{
    struct stat st;
    uint64_t k = 0xCBF29CE484222325ull;
    uint64_t v[8];
    memset(v, 0, sizeof v);
    if (stat(path, &st) != 0)
        return 0;
    v[0] = (uint64_t)st.st_size;
    v[1] = (uint64_t)st.st_mtime;
    v[2] = (uint64_t)alpha_mode | (uint64_t)fmt_pref << 8 | (uint64_t)CACHE_VERSION << 16;
    v[3] = (uint64_t)j->ow | (uint64_t)j->oh << 20 | (uint64_t)j->olevels << 40;
    v[5] = j->cut;
    v[4] = j->akey;
    k = fnv(k, path, strlen(path));
    k = fnv(k, v, sizeof v);
    return k ? k : 1;
}

static uint64_t alpha_key(const uint8_t *a, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;
    if (!a)
        return 0;
    for (i = 0; i < n; i += 1 << 20)
        h = nv2a_tex_hash(a + i, (uint32_t)(n - i < (1u << 20) ? n - i : (1u << 20)), h);
    return h;
}

/* BC for a pack that allows it, a backend that can, and a texture big
 * enough to matter (a small one stays raw: it is cheap and shows no loss). */
#define COMPRESS_MIN_TEXELS (128u * 128u)

static uint32_t pick_format(int pack_compress)
{
    unsigned m = atomic_load(&s_formats);
    if (pack_compress == NV2A_TEXPACK_COMPRESS_OFF || !nv2a_texpack_compress_on())
        return NV2A_TEXPACK_FMT_BGRA8;
    if ((m & (1u << NV2A_TEXPACK_FMT_BC3)) && (m & (1u << NV2A_TEXPACK_FMT_BC1)))
        return NV2A_TEXPACK_FMT_BC3;
    return NV2A_TEXPACK_FMT_BGRA8;
}

static void run_job(Job *j, char *path, int alpha_mode, int compress, const char *pname)
{
    uint32_t pw = 0, ph = 0, pref = pick_format(compress);
    int has_alpha = 0, ok = 0;
    uint64_t ckey = 0, t0 = xbox_HostNowNs();
    struct nv2a_texpack_img img;
    uint8_t *px;

    memset(&img, 0, sizeof img);
    if (pref != NV2A_TEXPACK_FMT_BGRA8) {
        ckey = job_key(path, j, alpha_mode, pref);
        if (ckey && cache_load(ckey, &img)) {
            j->img = img;
            j->from_cache = 1;
            j->cpu += img.bytes;
            pthread_mutex_lock(&s_mx);
            s_st.cpu_bytes += img.bytes;
            s_st.from_cache++;
            s_st.decode_ms += (double)(xbox_HostNowNs() - t0) / 1e6;
            pthread_mutex_unlock(&s_mx);
            j->state = J_READY;
            if (nv2a_texpack_log())
                fprintf(stderr, LOGP "%s: cache %ux%u x%u levels %s\n", path, img.w, img.h,
                        img.levels, img.fmt == NV2A_TEXPACK_FMT_BC1 ? "BC1" : "BC3");
            return;
        }
    }
    px = nv2a_texpack_png_read(path, &pw, &ph, &has_alpha);
    if (!px) {
        fprintf(stderr, LOGP "%s: not a PNG this build reads (or over %ux%u), the title's texture stays\n",
                path, NV2A_TEXPACK_MAX_SIDE, NV2A_TEXPACK_MAX_SIDE);
        pthread_mutex_lock(&s_mx);
        s_st.failed++;
        pthread_mutex_unlock(&s_mx);
        j->state = J_FAILED;
        return;
    }
    {
        uint64_t t1 = xbox_HostNowNs();
        uint32_t hint = pref != NV2A_TEXPACK_FMT_BGRA8 && (uint64_t)pw * ph >= COMPRESS_MIN_TEXELS
                            ? pref : NV2A_TEXPACK_FMT_BGRA8;
        ok = nv2a_texpack_build(px, pw, ph, has_alpha, alpha_mode, j->alpha, j->ow, j->oh,
                                j->olevels, j->cut, hint, &img);
        free(px);
        pthread_mutex_lock(&s_mx);
        s_st.decode_ms += (double)(t1 - t0) / 1e6;
        s_st.compress_ms += (double)(xbox_HostNowNs() - t1) / 1e6;
        pthread_mutex_unlock(&s_mx);
    }
    if (!ok) {
        fprintf(stderr, LOGP "%s: no memory for the replacement\n", path);
        pthread_mutex_lock(&s_mx);
        s_st.failed++;
        pthread_mutex_unlock(&s_mx);
        j->state = J_FAILED;
        return;
    }
    if (ckey && img.fmt != NV2A_TEXPACK_FMT_BGRA8)
        cache_save(ckey, &img);
    j->img = img;
    j->cpu += img.bytes;
    pthread_mutex_lock(&s_mx);
    s_st.cpu_bytes += img.bytes;
    s_st.decoded++;
    pthread_mutex_unlock(&s_mx);
    j->state = J_READY;
    if (nv2a_texpack_log())
        fprintf(stderr, LOGP "%s (%s): %ux%u -> %u levels %s, %.1f MB, %.0f ms\n", path, pname, pw,
                ph, img.levels, img.fmt == NV2A_TEXPACK_FMT_BGRA8 ? "BGRA8"
                : img.fmt == NV2A_TEXPACK_FMT_BC1 ? "BC1" : "BC3",
                (double)img.bytes / 1048576.0, (double)(xbox_HostNowNs() - t0) / 1e6);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_mx);
    for (;;) {
        int slot;
        Job *j;
        char path[1100], pname[96];
        int alpha_mode, compress, got;
        while (!s_qn)
            pthread_cond_wait(&s_cv, &s_mx);
        slot = s_queue[s_qh];
        s_qh = (s_qh + 1) % MAX_JOBS;
        s_qn--;
        j = &s_job[slot];
        if (j->state != J_QUEUED)
            continue;
        if (!j->refs) {                  /* nobody wants it any more */
            job_free_locked(j);
            continue;
        }
        j->state = J_RUNNING;
        s_busy++;
        pthread_mutex_unlock(&s_mx);
        got = entry_info(j->id, j->gen, path, sizeof path, &alpha_mode, &compress, pname, sizeof pname);
        if (got)
            run_job(j, path, alpha_mode, compress, pname);
        else
            j->state = J_FAILED;
        pthread_mutex_lock(&s_mx);
        s_busy--;
        if (!j->refs)
            job_free_locked(j);
        if (!s_qn && !s_busy)
            pthread_cond_broadcast(&s_idle);
    }
    return NULL;
}

static void start_workers(void)
{
    int i, n;
    long cores = 2;
#if defined(_SC_NPROCESSORS_ONLN)
    cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    n = cores >= 2 ? MAX_WORKERS : 1;
    for (i = 0; i < n; i++)
        if (pthread_create(&s_thr[i], NULL, worker, NULL) == 0)
            pthread_detach(s_thr[i]);
    s_started = 1;
}

uint32_t nv2a_texpack_request(const struct nv2a_texpack_req *r)
{
    int i, slot = -1;
    uint32_t token = 0;
    size_t an = (size_t)r->w * r->h;

    if (!r->id || !r->w || !r->h)
        return 0;
    pthread_mutex_lock(&s_mx);
    for (i = 0; i < MAX_JOBS; i++) {
        Job *j = &s_job[i];
        if (j->state == J_FREE) {
            if (slot < 0)
                slot = i;
        } else if (j->id == r->id && j->gen == r->gen && j->ow == r->w && j->oh == r->h
                   && j->olevels == r->levels && j->cut == r->cut && j->refs && j->state != J_FAILED) {
            j->refs++;
            token = (j->serial << 10) | (uint32_t)(i + 1);
            pthread_mutex_unlock(&s_mx);
            return token;
        }
    }
    s_st.requests++;
    if (slot < 0 || s_st.cpu_bytes + an > cpu_cap()) {
        pthread_mutex_unlock(&s_mx);
        return 0;
    }
    {
        Job *j = &s_job[slot];
        memset(j, 0, sizeof *j);
        if (r->alpha) {
            j->alpha = (uint8_t *)malloc(an);
            if (!j->alpha) {
                pthread_mutex_unlock(&s_mx);
                return 0;
            }
            memcpy(j->alpha, r->alpha, an);
            j->akey = alpha_key(j->alpha, an);
            j->cpu = an;
            s_st.cpu_bytes += an;
        }
        j->state = J_QUEUED;
        j->serial = ++s_serial & 0x3FFFFFu;
        if (!j->serial)
            j->serial = ++s_serial & 0x3FFFFFu;
        j->id = r->id;
        j->gen = r->gen;
        j->refs = 1;
        j->ow = r->w;
        j->oh = r->h;
        j->olevels = r->levels;
        j->cut = r->cut;
        s_queue[(s_qh + s_qn) % MAX_JOBS] = slot;
        s_qn++;
        s_st.queued = s_qn;
        token = (j->serial << 10) | (uint32_t)(slot + 1);
    }
    if (!s_started)
        start_workers();
    pthread_cond_signal(&s_cv);
    pthread_mutex_unlock(&s_mx);
    return token;
}

static Job *token_job(uint32_t token)
{
    uint32_t slot = (token & 0x3FFu);
    Job *j;
    if (!slot || slot > MAX_JOBS)
        return NULL;
    j = &s_job[slot - 1];
    return j->state != J_FREE && j->serial == token >> 10 ? j : NULL;
}

int nv2a_texpack_poll(uint32_t token, const struct nv2a_texpack_img **img)
{
    Job *j;
    int r = NV2A_TEXPACK_FAILED;
    pthread_mutex_lock(&s_mx);
    j = token_job(token);
    if (j) {
        if (j->gen && j->gen != atomic_load(&s_gen)) {
            r = NV2A_TEXPACK_FAILED;   /* made for packs that are gone */
        } else if (j->state == J_READY) {
            *img = &j->img;
            r = NV2A_TEXPACK_READY;
        } else if (j->state == J_QUEUED || j->state == J_RUNNING) {
            r = NV2A_TEXPACK_PENDING;
        }
    }
    s_st.queued = s_qn;
    pthread_mutex_unlock(&s_mx);
    return r;
}

void nv2a_texpack_release(uint32_t token)
{
    Job *j;
    pthread_mutex_lock(&s_mx);
    j = token_job(token);
    if (j && j->refs) {
        j->refs--;
        if (!j->refs && j->state != J_QUEUED && j->state != J_RUNNING)
            job_free_locked(j);
        /* A queued job is dropped by the worker that dequeues it; a running one when it ends. */
    }
    pthread_mutex_unlock(&s_mx);
}

int nv2a_texpack_flush(unsigned timeout_ms)
{
    struct timespec ts;
    int ok = 1;
    pthread_mutex_lock(&s_mx);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (s_qn || s_busy)
        if (pthread_cond_timedwait(&s_idle, &s_mx, &ts) != 0) {
            ok = 0;
            break;
        }
    pthread_mutex_unlock(&s_mx);
    return ok;
}

void nv2a_texpack_stats(struct nv2a_texpack_stats *s)
{
    pthread_mutex_lock(&s_mx);
    *s = s_st;
    pthread_mutex_unlock(&s_mx);
}

struct nv2a_texpack_stats *nv2a_texpack_stats_rw(void)
{
    return &s_st;
}

/* ── the dump ─────────────────────────────────────────────────────────── */

#define SEEN_N 65536u
static uint64_t *s_seen;

int nv2a_texpack_dump_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *d = recomp_env(RENV_TEXREP_DUMP);
        on = d && *d;
    }
    return on;
}

static int seen_add(uint64_t key)
{
    uint32_t i;
    if (!s_seen) {
        s_seen = (uint64_t *)calloc(SEEN_N, sizeof *s_seen);
        if (!s_seen)
            return 1;
    }
    key |= 1;
    for (i = (uint32_t)(key * 0x9E3779B97F4A7C15ull >> 48) & (SEEN_N - 1u);; i = (i + 1) & (SEEN_N - 1u)) {
        if (!s_seen[i]) {
            s_seen[i] = key;
            return 0;
        }
        if (s_seen[i] == key)
            return 1;
    }
}

void nv2a_texpack_dump(const uint8_t *guest, uint32_t color, uint32_t w, uint32_t h,
                       uint32_t pitch, uint32_t levels, const uint8_t *pal, uint32_t pal_len,
                       const uint32_t *texels)
{
    static const char *dir;
    static int init, mips;
    static unsigned count;
    uint64_t hash, palh;
    char name[64], path[1300];
    FILE *f;
    uint32_t l;

    if (!nv2a_texpack_dump_on() || !nv2a_tex_size_from_format(color))
        return;     /* only what a pack can replace: not a video frame or a screen copy */
    if (!init) {
        init = 1;
        dir = recomp_env(RENV_TEXREP_DUMP);
        mips = recomp_env(RENV_TEXREP_DUMP_MIPS) != NULL;
        mkdirs(dir);
        fprintf(stderr, LOGP "texrep_dump: writing every uploaded texture to %s as pack files\n", dir);
    }
    if (w > NV2A_TEXPACK_MAX_SIDE || h > NV2A_TEXPACK_MAX_SIDE
            || !nv2a_texpack_key(guest, color, w, h, pitch, pal, pal_len, &hash, &palh))
        return;
    if (seen_add(hash ^ palh * 31u))
        return;
    nv2a_texpack_key_name(hash, palh, name, sizeof name);
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (nv2a_texpack_png_write(path, (const uint8_t *)texels, w, h)) {
        count++;
        snprintf(path, sizeof path, "%s/index.tsv", dir);
        f = fopen(path, "ab");
        if (f) {
            fprintf(f, "%016llx\t%016llx\t%u\t%u\t0x%02x\t%u\n", (unsigned long long)hash,
                    (unsigned long long)palh, w, h, color & 0xFFu, levels);
            fclose(f);
        }
        if (count <= 4 || count % 100 == 0)
            fprintf(stderr, LOGP "texrep_dump: %s (%ux%u, format 0x%02x), %u so far\n", name, w, h,
                    color & 0xFFu, count);
    }
    texels += (size_t)w * h;
    for (l = 1; mips && l < levels; l++) {
        uint32_t lw = nv2a_tex_level_dim(w, l), lh = nv2a_tex_level_dim(h, l);
        char base[64];
        snprintf(base, sizeof base, "%s", name);
        base[strlen(base) - 4] = 0;
        snprintf(path, sizeof path, "%s/%s_m%u.png", dir, base, l);
        nv2a_texpack_png_write(path, (const uint8_t *)texels, lw, lh);
        texels += (size_t)lw * lh;
    }
}

/**
 * nv2a_texscale_store.c - scaled 2D art off the render thread and on disk.
 * See nv2a_texscale.h (the store).
 *
 * One worker thread takes jobs in order. A job is a copy of a texture's
 * decoded texels with its key; the worker looks for the scaled picture in the
 * disk cache, scales it when it is not there (and writes it), and leaves the
 * result for the backend to take at one of its next binds. The render thread
 * only copies texels in and results out.
 *
 * A cache file is one scaled level 0, raw A8R8G8B8, behind a header that
 * names what it is and carries a checksum of the texels:
 *
 *   <key>_<w>x<h>_x<factor>_<algo><version>.tsc
 *
 * written under another name and renamed, so a reader sees a whole file or
 * none. A file whose header, length or checksum is not what its name
 * promises is deleted and the texture is scaled again. NV2A_TEXSCALE_VERSION
 * is in the name and the header: a changed scaler leaves the old files to
 * the size bound, which deletes the files least recently used once the
 * directory passes it (a file's time is set to now whenever it is read, so
 * the art seen in every run is the last to go, whenever it was written).
 */
#include "nv2a_texscale.h"
#include "recomp_env.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t nv2a_texscale_key(const uint32_t *texels, uint32_t w, uint32_t h)
{
    /* Words through a multiply-xorshift mix; the size is part of it. */
    uint64_t x = 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 32 | h);
    size_t i, n = (size_t)w * h;
    for (i = 0; i < n; i++) {
        x ^= texels[i];
        x *= 0xFF51AFD7ED558CCDull;
        x ^= x >> 29;
    }
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x ? x : 1;
}

#if !defined(_MSC_VER)

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#define LOGP "[UISCALE] "
#define MAGIC 0x53545258u            /* "XRTS" */
#define JOBS_MAX 512
#define RESULTS_MAX 96
#define RESULT_BYTES_MAX (192u << 20) /* results not yet taken, in memory */

struct ts_header {
    uint32_t magic, version, w, h, factor, algo;
    uint64_t key;
    uint64_t checksum;               /* nv2a_texscale_key of the payload */
    uint32_t bytes, pad;
};

typedef struct {
    uint64_t  key;
    uint32_t  w, h, factor, algo;
    unsigned  flags;
    uint32_t *texels;                /* the job's copy of the source */
} Job;

typedef struct {
    uint64_t  key;
    uint32_t  w, h, factor, algo;
    uint32_t *texels;                /* NULL: the scaler refused */
    uint64_t  stamp;
    int       used;
} Result;

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cv = PTHREAD_COND_INITIALIZER;     /* a job came */
static pthread_cond_t  s_idle = PTHREAD_COND_INITIALIZER;   /* the queue ran out */
static Job      s_jobs[JOBS_MAX];
static unsigned s_job_head, s_job_n;
static int      s_busy;              /* the worker has a job in hand */
static Job      s_cur;               /* that job (its key and what it is) */
static Result   s_res[RESULTS_MAX];
static uint64_t s_res_bytes, s_stamp;
static int      s_started, s_sync;
static char     s_dir[900];
static int      s_dir_on, s_swept;
static uint64_t s_limit = 256ull << 20, s_dir_bytes;
static _Atomic unsigned s_batch_done, s_batch_total;
static _Atomic uint64_t s_hits, s_misses, s_written, s_bad, s_load_us, s_scale_us;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int log_on(void)
{
    return recomp_env(RENV_UI_SCALE_LOG) != NULL;
}

static const char *algo_tag(uint32_t algo)
{
    return algo == NV2A_TEXSCALE_BICUBIC ? "bc" : "sx";
}

static void file_path(char *out, size_t n, const char *dir, uint64_t key, uint32_t w,
                      uint32_t h, uint32_t factor, uint32_t algo)
{
    snprintf(out, n, "%s/%016llx_%ux%u_x%u_%s%u.tsc", dir, (unsigned long long)key, w, h,
             factor, algo_tag(algo), (unsigned)NV2A_TEXSCALE_VERSION);
}

/* ── the directory ───────────────────────────────────────────────────── */

void nv2a_texscale_cache_dir(const char *dir)
{
    pthread_mutex_lock(&s_mx);
    s_dir_on = 0;
    s_swept = 0;
    if (dir && *dir && strlen(dir) < sizeof s_dir - 80) {
        const char *e = recomp_env(RENV_UI_SCALE_CACHE_MB);
        snprintf(s_dir, sizeof s_dir, "%s", dir);
        s_dir_on = 1;
        if (e && *e)
            s_limit = (uint64_t)(atof(e) > 0 ? atof(e) : 0) * 1048576u;
        if (e && !strcmp(e, "0"))
            s_dir_on = 0;            /* =0: no disk cache */
    }
    /* Said once a change, without any trace switch: a backend names the
     * directory when the setting is first on, so this is the line that says
     * the 2D art is being scaled and where its pictures are kept. */
    if (s_dir_on)
        fprintf(stderr, LOGP "2D art upscaling on (%ux, %s); scaled textures kept in %s"
                " (at most %.0f MB)\n", nv2a_texscale_factor(),
                nv2a_texscale_algo_name(nv2a_texscale_algo()), s_dir,
                (double)s_limit / 1048576.0);
    else if (dir && *dir)
        fprintf(stderr, LOGP "2D art upscaling on (%ux, %s); no disk cache\n",
                nv2a_texscale_factor(), nv2a_texscale_algo_name(nv2a_texscale_algo()));
    pthread_mutex_unlock(&s_mx);
}

int nv2a_texscale_cache_path(char *out, size_t n)
{
    int on;
    pthread_mutex_lock(&s_mx);
    on = s_dir_on;
    if (on && out)
        snprintf(out, n, "%s", s_dir);
    pthread_mutex_unlock(&s_mx);
    return on;
}

typedef struct {
    char     name[96];
    uint64_t bytes;
    time_t   mtime;
} Ent;

static int ent_older(const void *a, const void *b)
{
    const Ent *x = (const Ent *)a, *y = (const Ent *)b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime ? 1 : strcmp(x->name, y->name);
}

/* The directory's size; past the bound the least recently used files go
 * until three quarters of it are left. Stale temporaries go too. Worker
 * thread (or a test), dir fixed by the caller. */
static void dir_sweep(const char *dir, uint64_t limit)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    Ent *ents = NULL;
    size_t n = 0, cap = 0, i;
    uint64_t total = 0;
    time_t now = time(NULL);

    if (!d) {
        s_dir_bytes = 0;
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char path[1100];
        struct stat st;
        size_t len = strlen(de->d_name);
        int tmp = len > 4 && !strcmp(de->d_name + len - 4, ".tmp");
        if (len >= sizeof ents->name || len < 5
                || (!tmp && strcmp(de->d_name + len - 4, ".tsc")))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (tmp) {
            if (now - st.st_mtime > 600)
                remove(path);        /* a writer that died */
            continue;
        }
        if (n == cap) {
            Ent *p = (Ent *)realloc(ents, (cap ? cap * 2 : 256) * sizeof *ents);
            if (!p)
                break;
            ents = p;
            cap = cap ? cap * 2 : 256;
        }
        snprintf(ents[n].name, sizeof ents[n].name, "%s", de->d_name);
        ents[n].bytes = (uint64_t)st.st_size;
        ents[n].mtime = st.st_mtime;
        total += ents[n].bytes;
        n++;
    }
    closedir(d);
    if (total > limit) {
        uint64_t before = total;
        size_t gone = 0;
        qsort(ents, n, sizeof *ents, ent_older);
        for (i = 0; i < n && total > limit / 4 * 3; i++) {
            char path[1100];
            snprintf(path, sizeof path, "%s/%s", dir, ents[i].name);
            if (remove(path) == 0) {
                total -= ents[i].bytes;
                gone++;
            }
        }
        fprintf(stderr, LOGP "cache %s: %.1f MB over the %.0f MB bound, %zu files removed,"
                " %.1f MB left\n", dir, (double)before / 1048576.0,
                (double)limit / 1048576.0, gone, (double)total / 1048576.0);
    }
    free(ents);
    s_dir_bytes = total;
}

static void mkdirs(const char *dir)
{
    char p[1000];
    size_t i, n = strlen(dir);
    if (n >= sizeof p)
        return;
    memcpy(p, dir, n + 1);
    for (i = 1; i < n; i++) {
        if (p[i] == '/' || p[i] == '\\') {
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
#if defined(_WIN32)
    mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

/* 1: dst holds the scaled level 0. 0: no file, or not a good one (deleted). */
static int file_load(const char *dir, const Job *j, uint32_t *dst)
{
    char path[1100];
    struct ts_header hd;
    uint32_t bytes = j->w * j->factor * j->h * j->factor * 4u;
    FILE *f;
    int ok = 0;

    file_path(path, sizeof path, dir, j->key, j->w, j->h, j->factor, j->algo);
    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(&hd, 1, sizeof hd, f) == sizeof hd && hd.magic == MAGIC
            && hd.version == NV2A_TEXSCALE_VERSION && hd.w == j->w && hd.h == j->h
            && hd.factor == j->factor && hd.algo == j->algo && hd.key == j->key
            && hd.bytes == bytes && fread(dst, 1, bytes, f) == bytes
            && fgetc(f) == EOF
            && nv2a_texscale_key(dst, j->w * j->factor, j->h * j->factor) == hd.checksum)
        ok = 1;
    fclose(f);
    if (!ok) {
        atomic_fetch_add_explicit(&s_bad, 1, memory_order_relaxed);
        fprintf(stderr, LOGP "cache file %s is cut or damaged: removed, scaling again\n",
                path);
        remove(path);
        return 0;
    }
    utime(path, NULL);               /* used now: the last to be removed */
    return 1;
}

static void file_store(const char *dir, const Job *j, const uint32_t *texels)
{
    char path[1100], tmp[1200];
    struct ts_header hd;
    uint32_t W = j->w * j->factor, H = j->h * j->factor, bytes = W * H * 4u;
    FILE *f;

    memset(&hd, 0, sizeof hd);
    hd.magic = MAGIC;
    hd.version = NV2A_TEXSCALE_VERSION;
    hd.w = j->w;
    hd.h = j->h;
    hd.factor = j->factor;
    hd.algo = j->algo;
    hd.key = j->key;
    hd.checksum = nv2a_texscale_key(texels, W, H);
    hd.bytes = bytes;
    file_path(path, sizeof path, dir, j->key, j->w, j->h, j->factor, j->algo);
    snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid());
    f = fopen(tmp, "wb");
    if (!f) {
        mkdirs(dir);
        f = fopen(tmp, "wb");
    }
    if (!f)
        return;
    if (fwrite(&hd, 1, sizeof hd, f) != sizeof hd || fwrite(texels, 1, bytes, f) != bytes) {
        fclose(f);
        remove(tmp);
        return;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return;
    }
#if defined(_WIN32)
    remove(path);                    /* rename does not replace there */
#endif
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return;
    }
    atomic_fetch_add_explicit(&s_written, 1, memory_order_relaxed);
    s_dir_bytes += bytes + sizeof hd;
    if (s_dir_bytes > s_limit)
        dir_sweep(dir, s_limit);
}

/* ── results ─────────────────────────────────────────────────────────── */

static int same(const Result *r, uint64_t key, uint32_t w, uint32_t h, uint32_t factor,
                uint32_t algo)
{
    return r->used && r->key == key && r->w == w && r->h == h && r->factor == factor
        && r->algo == algo;
}

/* s_mx held. texels is handed over (or NULL: failed). A full table, or one
 * holding more than RESULT_BYTES_MAX, drops what was waiting longest: a
 * result nobody came for (its texture left the backend's cache). */
static void result_put(const Job *j, uint32_t *texels)
{
    uint64_t bytes = texels ? (uint64_t)j->w * j->h * j->factor * j->factor * 4u : 0;
    Result *slot = NULL;
    int i;

    for (;;) {
        Result *old = NULL;
        slot = NULL;
        for (i = 0; i < RESULTS_MAX; i++) {
            Result *r = &s_res[i];
            if (!r->used) {
                if (!slot)
                    slot = r;
            } else if (!old || r->stamp < old->stamp) {
                old = r;
            }
        }
        if (slot && (s_res_bytes + bytes <= RESULT_BYTES_MAX || !old))
            break;
        if (!old)
            break;
        if (old->texels)
            s_res_bytes -= (uint64_t)old->w * old->h * old->factor * old->factor * 4u;
        free(old->texels);
        memset(old, 0, sizeof *old);
    }
    if (!slot) {
        free(texels);
        return;
    }
    slot->key = j->key;
    slot->w = j->w;
    slot->h = j->h;
    slot->factor = j->factor;
    slot->algo = j->algo;
    slot->texels = texels;
    slot->stamp = ++s_stamp;
    slot->used = 1;
    s_res_bytes += bytes;
}

/* ── the worker ──────────────────────────────────────────────────────── */

static void job_run(const Job *j)
{
    char dir[sizeof s_dir];
    uint32_t W = j->w * j->factor, H = j->h * j->factor;
    uint32_t *out = (uint32_t *)malloc((size_t)W * H * 4);
    uint64_t t0 = now_us(), t1;
    int on, hit = 0, ok = 0, swept;

    pthread_mutex_lock(&s_mx);
    on = s_dir_on;
    memcpy(dir, s_dir, sizeof dir);
    swept = s_swept;
    s_swept = 1;
    pthread_mutex_unlock(&s_mx);
    if (on && !swept)
        dir_sweep(dir, s_limit);
    if (out && on)
        hit = file_load(dir, j, out);
    if (hit) {
        ok = 1;
        t1 = now_us();
        atomic_fetch_add_explicit(&s_hits, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_load_us, t1 - t0, memory_order_relaxed);
    } else if (out) {
        ok = nv2a_texscale(j->texels, j->w, j->h, j->factor,
                           (enum nv2a_texscale_algo)j->algo, out);
        t1 = now_us();
        atomic_fetch_add_explicit(&s_misses, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s_scale_us, t1 - t0, memory_order_relaxed);
        if (ok) {
            nv2a_texscale_note(j->w, j->h, j->factor, t1 - t0);
            if (on)
                file_store(dir, j, out);
        }
    } else {
        t1 = t0;
    }
    if (log_on())
        fprintf(stderr, LOGP "job %016llx %ux%u x%u: %s in %.2f ms%s\n",
                (unsigned long long)j->key, j->w, j->h, j->factor,
                hit ? "loaded from the cache" : ok ? "scaled" : "failed",
                (double)(t1 - t0) / 1000.0,
                (j->flags & NV2A_TEXSCALE_DISK_ONLY) ? " (ahead of use)" : "");
    if (!ok) {
        free(out);
        out = NULL;
    }
    pthread_mutex_lock(&s_mx);
    /* A job queued ahead of use leaves its result on disk only, unless a
     * bind asked for the same texture meanwhile (s_cur's flags). */
    if (s_cur.flags & NV2A_TEXSCALE_DISK_ONLY)
        free(out);
    else
        result_put(j, out);
    pthread_mutex_unlock(&s_mx);
}

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&s_mx);
    for (;;) {
        Job j;
        while (!s_job_n) {
            s_busy = 0;
            pthread_cond_broadcast(&s_idle);
            pthread_cond_wait(&s_cv, &s_mx);
        }
        j = s_jobs[s_job_head];
        s_job_head = (s_job_head + 1) % JOBS_MAX;
        s_job_n--;
        s_busy = 1;
        s_cur = j;
        pthread_mutex_unlock(&s_mx);
        job_run(&j);
        free(j.texels);
        atomic_fetch_add_explicit(&s_batch_done, 1, memory_order_relaxed);
        pthread_mutex_lock(&s_mx);
        memset(&s_cur, 0, sizeof s_cur);
    }
    return NULL;
}

/* ── the calls ───────────────────────────────────────────────────────── */

void nv2a_texscale_set_sync(int on)
{
    s_sync = on != 0;
}

int nv2a_texscale_get(uint64_t key, const uint32_t *texels, uint32_t w, uint32_t h,
                      unsigned factor, enum nv2a_texscale_algo algo, unsigned flags,
                      uint32_t *dst)
{
    unsigned i;
    int queued = 0;

    if (!key || !w || !h || (factor != 2 && factor != 4)
            || (unsigned)algo >= NV2A_TEXSCALE_ALGOS)
        return NV2A_TEXSCALE_MISS;
    pthread_mutex_lock(&s_mx);
    if (dst && !(flags & NV2A_TEXSCALE_DISK_ONLY)) {
        for (i = 0; i < RESULTS_MAX; i++) {
            Result *r = &s_res[i];
            if (same(r, key, w, h, factor, (uint32_t)algo)) {
                int ok = r->texels != NULL;
                if (ok) {
                    memcpy(dst, r->texels, (size_t)w * h * factor * factor * 4);
                    s_res_bytes -= (uint64_t)w * h * factor * factor * 4u;
                }
                free(r->texels);
                memset(r, 0, sizeof *r);
                pthread_mutex_unlock(&s_mx);
                return ok ? NV2A_TEXSCALE_READY : NV2A_TEXSCALE_FAILED;
            }
        }
    }
    /* In the worker's hands or in the queue already: a bind's request makes
     * a job queued ahead of use keep its result. */
    if (s_busy && s_cur.key == key && s_cur.w == w && s_cur.h == h && s_cur.factor == factor
            && s_cur.algo == (uint32_t)algo) {
        s_cur.flags &= flags | ~(unsigned)NV2A_TEXSCALE_DISK_ONLY;
        queued = 1;
    }
    for (i = 0; i < s_job_n && !queued; i++) {
        Job *j = &s_jobs[(s_job_head + i) % JOBS_MAX];
        if (j->key == key && j->w == w && j->h == h && j->factor == factor
                && j->algo == (uint32_t)algo) {
            j->flags &= flags | ~(unsigned)NV2A_TEXSCALE_DISK_ONLY;
            queued = 1;
        }
    }
    if (queued) {
        pthread_mutex_unlock(&s_mx);
        return NV2A_TEXSCALE_PENDING;
    }
    if (!texels) {
        pthread_mutex_unlock(&s_mx);
        return NV2A_TEXSCALE_MISS;
    }
    if ((flags & NV2A_TEXSCALE_DISK_ONLY) && !s_dir_on) {
        pthread_mutex_unlock(&s_mx);
        return NV2A_TEXSCALE_MISS;   /* nowhere to keep it */
    }
    if (s_sync && dst && !(flags & NV2A_TEXSCALE_DISK_ONLY)) {
        /* A test's (or a tool's) one-thread mode: the whole job here. */
        Job j = { key, w, h, factor, (uint32_t)algo, flags, (uint32_t *)texels };
        s_cur = j;
        pthread_mutex_unlock(&s_mx);
        job_run(&j);
        pthread_mutex_lock(&s_mx);
        memset(&s_cur, 0, sizeof s_cur);
        pthread_mutex_unlock(&s_mx);
        return nv2a_texscale_get(key, NULL, w, h, factor, algo, flags, dst)
               == NV2A_TEXSCALE_READY ? NV2A_TEXSCALE_READY : NV2A_TEXSCALE_FAILED;
    }
    if (s_job_n == JOBS_MAX) {
        pthread_mutex_unlock(&s_mx);
        return NV2A_TEXSCALE_MISS;
    }
    {
        Job *j = &s_jobs[(s_job_head + s_job_n) % JOBS_MAX];
        j->texels = (uint32_t *)malloc((size_t)w * h * 4);
        if (!j->texels) {
            pthread_mutex_unlock(&s_mx);
            return NV2A_TEXSCALE_MISS;
        }
        memcpy(j->texels, texels, (size_t)w * h * 4);
        j->key = key;
        j->w = w;
        j->h = h;
        j->factor = factor;
        j->algo = (uint32_t)algo;
        j->flags = flags;
        if (!s_job_n && !s_busy) {
            /* a new batch: the count starts over */
            atomic_store_explicit(&s_batch_done, 0, memory_order_relaxed);
            atomic_store_explicit(&s_batch_total, 0, memory_order_relaxed);
        }
        s_job_n++;
        atomic_fetch_add_explicit(&s_batch_total, 1, memory_order_relaxed);
    }
    if (!s_started) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, worker, NULL) != 0) {
            /* No thread: the job is taken back and the texture stays as the
             * title has it. */
            pthread_attr_destroy(&at);
            s_job_n--;
            free(s_jobs[(s_job_head + s_job_n) % JOBS_MAX].texels);
            atomic_fetch_sub_explicit(&s_batch_total, 1, memory_order_relaxed);
            pthread_mutex_unlock(&s_mx);
            return NV2A_TEXSCALE_MISS;
        }
        pthread_attr_destroy(&at);
        s_started = 1;
        s_busy = 1;                  /* until it has looked at the queue */
    }
    pthread_cond_signal(&s_cv);
    pthread_mutex_unlock(&s_mx);
    return NV2A_TEXSCALE_PENDING;
}

int nv2a_texscale_flush(unsigned timeout_ms)
{
    struct timespec until;
    int done;

    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += timeout_ms / 1000u;
    until.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s_mx);
    while (s_started && (s_job_n || s_busy)) {
        if (pthread_cond_timedwait(&s_idle, &s_mx, &until) == ETIMEDOUT)
            break;
    }
    done = !s_started || (!s_job_n && !s_busy);
    pthread_mutex_unlock(&s_mx);
    return done;
}

void nv2a_texscale_progress(unsigned *done, unsigned *total)
{
    unsigned d = atomic_load_explicit(&s_batch_done, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s_batch_total, memory_order_relaxed);
    if (d > t)
        d = t;
    if (done)
        *done = d;
    if (total)
        *total = t;
}

void nv2a_texscale_store_stats(struct nv2a_texscale_store_stats *st)
{
    memset(st, 0, sizeof *st);
    st->hits = atomic_load_explicit(&s_hits, memory_order_relaxed);
    st->misses = atomic_load_explicit(&s_misses, memory_order_relaxed);
    st->written = atomic_load_explicit(&s_written, memory_order_relaxed);
    st->bad = atomic_load_explicit(&s_bad, memory_order_relaxed);
    st->load_usec = atomic_load_explicit(&s_load_us, memory_order_relaxed);
    st->scale_usec = atomic_load_explicit(&s_scale_us, memory_order_relaxed);
    st->dir_bytes = s_dir_bytes;
}

void nv2a_texscale_cache_trim(uint64_t limit_bytes)
{
    char dir[sizeof s_dir];
    int on;
    pthread_mutex_lock(&s_mx);
    on = s_dir_on;
    memcpy(dir, s_dir, sizeof dir);
    pthread_mutex_unlock(&s_mx);
    if (on)
        dir_sweep(dir, limit_bytes);
}

#else  /* _MSC_VER: no worker and no disk cache; a request scales in place */

void nv2a_texscale_cache_dir(const char *dir) { (void)dir; }
int nv2a_texscale_cache_path(char *out, size_t n) { (void)out; (void)n; return 0; }
void nv2a_texscale_set_sync(int on) { (void)on; }

int nv2a_texscale_get(uint64_t key, const uint32_t *texels, uint32_t w, uint32_t h,
                      unsigned factor, enum nv2a_texscale_algo algo, unsigned flags,
                      uint32_t *dst)
{
    (void)key;
    if (!texels || !dst || (flags & NV2A_TEXSCALE_DISK_ONLY))
        return NV2A_TEXSCALE_MISS;
    return nv2a_texscale(texels, w, h, factor, algo, dst) ? NV2A_TEXSCALE_READY
                                                          : NV2A_TEXSCALE_FAILED;
}

int nv2a_texscale_flush(unsigned timeout_ms) { (void)timeout_ms; return 1; }

void nv2a_texscale_progress(unsigned *done, unsigned *total)
{
    if (done) *done = 0;
    if (total) *total = 0;
}

void nv2a_texscale_store_stats(struct nv2a_texscale_store_stats *st)
{
    memset(st, 0, sizeof *st);
}

void nv2a_texscale_cache_trim(uint64_t limit_bytes) { (void)limit_bytes; }

#endif

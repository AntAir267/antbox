/*
 * nvr_capture.c - RECOMP_NVR_CAPTURE=<file> (nvr_capture.h).
 *
 * Which pages changed: userfaultfd's asynchronous write protection. Every
 * view of guest memory is registered and write-protected; a write (the
 * title's, or the kernel's on its behalf, a file read into it) unprotects
 * its page and marks it written, with no fault handler. Each kick asks
 * PAGEMAP_SCAN for the written pages and protects them again in the same
 * call, then copies them: a write after that marks its page for the next
 * kick, so none is lost. A page written through several views (a mirror,
 * the tiled aperture) is copied once, from its space's primary view.
 *
 * The first version read soft-dirty bits and cleared them with clear_refs
 * after the scan; a page written between the two lost its mark, and
 * MechAssault 2's push buffer came out holed (its replay's walks stopped on
 * stale words; tools/nvr_replay's nvr_capture_test loses thousands of
 * writes that way). Without userfaultfd's async mode (Linux before 6.7)
 * there is no capture; RECOMP_NVR_CAPTURE_SOFTDIRTY=1 forces the old path,
 * for the test.
 *
 * RECOMP_NVR_CAPTURE_MB caps the file (default 4096): past it the capture
 * ends cleanly and the title runs on. The file is flushed at every kick,
 * so a run killed by a signal leaves it whole up to its last kick.
 */
#include "nvr_capture.h"

#if defined(__linux__)

#include "xbox_gamma.h"
#include "xbox_memory_layout.h"
#include "nv2a_pb_state.h"
#include "nv2a_backend_common.h"
#include "platform/recomp_env.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define LOGP "[nvr-capture] "
#define MAX_VIEWS 40
#define SPACES 3
#define VEC 512

static int s_state;                  /* 0 unread, 1 on, -1 off */
static FILE *s_f;
static int s_pm = -1, s_clear = -1, s_uffd = -1;
static xbox_mem_view s_v[MAX_VIEWS];
static int s_nv;
static const xbox_mem_view *s_prim[SPACES];
static uint8_t *s_mark[SPACES];      /* a page queued this kick */
static uint8_t *s_shadow[SPACES];    /* each page as the file last wrote it */
static uint32_t *s_list;             /* queued pages: space << 28 | page */
static size_t s_nlist, s_cap;
static uint64_t s_bytes, s_limit, s_kicks, s_pages, s_seen, s_scan_ns, s_swms, s_gpu_pages;
static uint64_t s_next_stats;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_state_last[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
static uint32_t s_gamma_gen = 0xFFFFFFFFu;
static uint32_t s_hud_last[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

/* Each runtime setting's tier, as recomp_env.h lists them. */
#define NVR_TIER_(id, tier, name, legacy, help) #tier,
static const char *const s_tier[] = { RECOMP_ENV_KEYS(NVR_TIER_) };
#undef NVR_TIER_

extern uint32_t xbox_ContiguousAllocatedBytes(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

static void put32(uint32_t v) { fwrite(&v, 4, 1, s_f); s_bytes += 4; }

static void end_file(const char *why)
{
    if (!s_f)
        return;
    put32(NVRCAP_END);
    fclose(s_f);
    s_f = NULL;
    s_state = -1;
    fprintf(stderr, LOGP "closed (%s): %llu kicks, %llu software methods, %llu pages written"
                    " of %llu seen, %.1f MB; %.1f ms finding written pages (%.1f us a scan)\n",
            why, (unsigned long long)s_kicks, (unsigned long long)s_swms,
            (unsigned long long)s_pages, (unsigned long long)s_seen, s_bytes / 1048576.0,
            s_scan_ns / 1e6, s_scan_ns / 1e3 / (double)(s_kicks + s_swms + 1));
}

static void at_exit(void) { nvr_capture_close(); }

/* Every view registered for asynchronous write protection and protected:
 * the fd, or -1 (then the soft-dirty fallback). */
static int uffd_setup(void)
{
    struct uffdio_api api;
    int u, i;

    u = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (u < 0)
        return -1;
    memset(&api, 0, sizeof api);
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM
                 | UFFD_FEATURE_WP_UNPOPULATED;
    if (ioctl(u, UFFDIO_API, &api) != 0) {
        close(u);
        return -1;
    }
    for (i = 0; i < s_nv; i++) {
        struct uffdio_register r;
        struct uffdio_writeprotect w;
        memset(&r, 0, sizeof r);
        r.range.start = (uint64_t)(uintptr_t)s_v[i].host;
        r.range.len = s_v[i].size;
        r.mode = UFFDIO_REGISTER_MODE_WP;
        memset(&w, 0, sizeof w);
        w.range = r.range;
        w.mode = UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(u, UFFDIO_REGISTER, &r) != 0 || ioctl(u, UFFDIO_WRITEPROTECT, &w) != 0) {
            fprintf(stderr, LOGP "userfaultfd: view %d (%p, %zu bytes) not protected\n",
                    i, s_v[i].host, s_v[i].size);
            close(u);
            return -1;
        }
    }
    return u;
}

static int start(void)
{
    const char *path = getenv("RECOMP_NVR_CAPTURE"), *mb = getenv("RECOMP_NVR_CAPTURE_MB");
    int i;

    if (!path || !*path)
        return -1;
    s_nv = xbox_MemoryViews(s_v, MAX_VIEWS);
    for (i = 0; i < s_nv; i++)
        if (s_v[i].space >= 0 && s_v[i].space < SPACES && !s_prim[s_v[i].space])
            s_prim[s_v[i].space] = &s_v[i];
    for (i = 0; i < SPACES; i++) {
        if (!s_prim[i])
            continue;
        s_mark[i] = (uint8_t *)calloc(s_prim[i]->size / NVRCAP_PAGE_SIZE, 1);
        s_shadow[i] = (uint8_t *)calloc(s_prim[i]->size, 1);
        if (!s_mark[i] || !s_shadow[i])
            return -1;
    }
    s_pm = open("/proc/self/pagemap", O_RDONLY);
    if (s_pm < 0) {
        fprintf(stderr, LOGP "off: /proc/self/pagemap not available\n");
        return -1;
    }
    /* RECOMP_NVR_CAPTURE_SOFTDIRTY=1: the fallback on purpose (its test). */
    s_uffd = getenv("RECOMP_NVR_CAPTURE_SOFTDIRTY") ? -1 : uffd_setup();
    if (s_uffd < 0) {
        /* The soft-dirty path loses pages (nvr_capture_test shows thousands):
         * a capture it makes would replay wrong frames, so it runs only when
         * asked for by name. */
        if (!getenv("RECOMP_NVR_CAPTURE_SOFTDIRTY")) {
            fprintf(stderr, LOGP "off: userfaultfd's async write protection is not"
                            " available (Linux 6.7 or later)\n");
            return -1;
        }
        s_clear = open("/proc/self/clear_refs", O_WRONLY);
        if (s_clear < 0) {
            fprintf(stderr, LOGP "off: clear_refs not available\n");
            return -1;
        }
        fprintf(stderr, LOGP "soft-dirty bits, as asked: pages written mid-scan are lost\n");
    }
    s_f = fopen(path, "wb");
    if (!s_f) {
        fprintf(stderr, LOGP "off: cannot write %s\n", path);
        return -1;
    }
    setvbuf(s_f, NULL, _IOFBF, 1 << 22);
    s_limit = (uint64_t)(mb ? atoll(mb) : 4096) << 20;
    fwrite(NVRCAP_MAGIC, 8, 1, s_f);
    s_bytes = 8;
    for (i = 0; i < SPACES; i++) {
        uint64_t sz;
        if (!s_prim[i])
            continue;
        sz = s_prim[i]->size;
        put32(NVRCAP_SPACE);
        put32((uint32_t)i);
        put32(s_prim[i]->guest);
        fwrite(&sz, 8, 1, s_f);
        s_bytes += 8;
    }
    atexit(at_exit);
    fprintf(stderr, LOGP "on: %s, %d views, cap %llu MB, %s\n", path, s_nv,
            (unsigned long long)(s_limit >> 20),
            s_uffd >= 0 ? "userfaultfd async write protection" : "soft-dirty bits");
    return 1;
}

static void queue(int space, size_t page)
{
    if (s_mark[space][page])
        return;
    if (s_nlist == s_cap) {
        size_t cap = s_cap ? s_cap * 2 : 4096;
        uint32_t *l = (uint32_t *)realloc(s_list, cap * sizeof *l);
        if (!l)
            return;
        s_list = l;
        s_cap = cap;
    }
    s_mark[space][page] = 1;
    s_list[s_nlist++] = (uint32_t)space << 28 | (uint32_t)page;
}

/* Every page of every view written since the last kick, queued. */
static void scan_written_in(int contig_only);
/* The RAM mirrors (28 views, 1.75 GB of page tables once write-protected)
 * cost most of a scan, and titles hardly write through them: they are
 * scanned every MIRROR_EVERY scans. A write there stays marked until then,
 * so none is lost; it lands in the file late, which is said once. */
#define MIRROR_EVERY 32

/* contig_only: the contiguous window and the tiled aperture over it, up to
 * the contiguous high-water mark, and the NV2A registers: where the push
 * buffer, the GPU's data and the engine state the walk reads back live,
 * and all a software method's dispatcher is looked at for (a mission
 * of Wreckless runs 4-5k of them a second; a whole scan each took half of
 * every second). Its other writes reach the file at the next kick. */
static void scan_written_in(int contig_only)
{
    static unsigned scans, said;
    struct page_region vec[VEC];
    int i, mirrors = !contig_only && scans++ % MIRROR_EVERY == 0;
    uint64_t high = xbox_ContiguousAllocatedBytes();

    for (i = 0; i < s_nv; i++) {
        uint64_t start = (uint64_t)(uintptr_t)s_v[i].host;
        uint64_t end = start + s_v[i].size;
        int mirror = s_v[i].space == 0 && &s_v[i] != s_prim[0];
        size_t before = s_nlist;
        if (contig_only) {
            /* And the NV2A registers: Wreckless's method 3 rewrites
             * DMA_SUBROUTINE (0xFD00324C), which the walk reads back right
             * after the method (nv2a_pb_scan.c, subr_adopt). */
            if (s_v[i].space == 0)
                continue;
            if (s_v[i].space == 1 && high && high < s_v[i].size)
                end = start + ((high + NVRCAP_PAGE_SIZE - 1) & ~(uint64_t)(NVRCAP_PAGE_SIZE - 1));
        }
        if (mirror && !mirrors)
            continue;
        while (start < end) {
            struct pm_scan_arg a;
            long n, k;
            memset(&a, 0, sizeof a);
            a.size = sizeof a;
            a.start = start;
            a.end = end;
            a.vec = (uint64_t)(uintptr_t)vec;
            a.vec_len = VEC;
            if (s_uffd >= 0) {
                /* The written pages, protected again in the same call. */
                a.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
                a.category_mask = PAGE_IS_WRITTEN;
                a.return_mask = PAGE_IS_WRITTEN;
            } else {
                a.category_mask = PAGE_IS_SOFT_DIRTY;
                a.return_mask = PAGE_IS_SOFT_DIRTY;
            }
            n = ioctl(s_pm, PAGEMAP_SCAN, &a);
            if (n < 0)
                break;
            for (k = 0; k < n; k++) {
                uint64_t p;
                for (p = vec[k].start; p < vec[k].end; p += NVRCAP_PAGE_SIZE)
                    queue(s_v[i].space,
                          (size_t)((p - (uint64_t)(uintptr_t)s_v[i].host) / NVRCAP_PAGE_SIZE));
            }
            if (a.walk_end <= start)
                break;
            start = a.walk_end;
        }
        if (mirror && s_nlist != before && !said++)
            fprintf(stderr, LOGP "the title writes through a RAM mirror (view at 0x%08X):"
                            " those pages reach the file up to %d kicks late\n",
                    s_v[i].guest, MIRROR_EVERY);
    }
}

/* The page as it stands, written when it differs from what the file last
 * gave it (the shadow: zero at first, as a replay's memory starts). Most
 * written pages come back with the same bytes -- MechAssault 2 rewrites
 * ~150 a kick -- so the shadow is what keeps a capture's size down. It
 * assumes a replay's memory is the file's: true where only the title
 * writes, and the executor's own writes (reports, semaphores, write-backs)
 * repeat in the replay as they happened live; the register window, which
 * the walk itself rewrites between a title's writes, is always written. */
static void write_page(int space, size_t page)
{
    const uint8_t *src = (const uint8_t *)s_prim[space]->host + page * NVRCAP_PAGE_SIZE;
    uint8_t *sh = s_shadow[space] + page * NVRCAP_PAGE_SIZE;
    uint8_t now[NVRCAP_PAGE_SIZE];

    memcpy(now, src, sizeof now);            /* one copy: the title may be writing */
    s_seen++;
    /* Not the NV2A registers: the executor writes them itself (the walk
     * publishes DMA_SUBROUTINE at every call), so a replay's copy can hold
     * the executor's value where the file's last one matched the title's
     * -- Wreckless's replay kept "in a call" and stopped on the next one. */
    if (space != 2 && !memcmp(now, sh, sizeof now))
        return;
    memcpy(sh, now, sizeof now);
    put32(NVRCAP_PAGE);
    put32((uint32_t)space);
    put32((uint32_t)(page * NVRCAP_PAGE_SIZE));
    fwrite(now, NVRCAP_PAGE_SIZE, 1, s_f);
    s_bytes += NVRCAP_PAGE_SIZE;
    s_pages++;
}

/* The written pages queued, copied and written (the kick's work without
 * the kick). Under s_mu. */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void flush_written_in(int contig_only)
{
    size_t i;
    uint64_t t0 = now_ns();
    scan_written_in(contig_only);
    s_scan_ns += now_ns() - t0;
    if (s_clear >= 0 && write(s_clear, "4", 1) != 1)
        fprintf(stderr, LOGP "clear_refs failed\n");
    for (i = 0; i < s_nlist; i++) {
        int sp = (int)(s_list[i] >> 28);
        size_t pg = s_list[i] & 0x0FFFFFFFu;
        write_page(sp, pg);
        s_mark[sp][pg] = 0;
    }
    s_nlist = 0;
}

void nvr_capture_kick(uint32_t from, uint32_t to)
{
    if (s_state < 0)
        return;
    pthread_mutex_lock(&s_mu);
    if (s_state == 0) {
        s_state = start();
        if (s_state > 0) {
            /* The first kick: every non-zero page as it stands (the replay
             * starts from zeroed memory). */
            int sp;
            if (s_clear >= 0 && write(s_clear, "4", 1) != 1)
                fprintf(stderr, LOGP "clear_refs failed\n");
            {
                /* The run's settings, for the replay's backend. */
                int id;
                for (id = 0; id < RENV_COUNT && id < (int)(sizeof s_tier / sizeof s_tier[0]); id++) {
                    const char *v = recomp_env((recomp_env_id)id);
                    char kv[512];
                    int n;
                    if (!v || strcmp(s_tier[id], "CONFIG"))
                        continue;
                    n = snprintf(kv, sizeof kv, "%s=%s", recomp_env_name((recomp_env_id)id), v);
                    if (n <= 0 || n >= (int)sizeof kv)
                        continue;
                    put32(NVRCAP_ENV);
                    put32((uint32_t)n);
                    fwrite(kv, (size_t)n, 1, s_f);
                    s_bytes += (uint64_t)n;
                }
            }
            for (sp = 0; sp < SPACES; sp++) {
                size_t n, pg;
                if (!s_prim[sp])
                    continue;
                n = s_prim[sp]->size / NVRCAP_PAGE_SIZE;
                for (pg = 0; pg < n; pg++)
                    write_page(sp, pg);        /* zero pages match the shadow */
            }
        }
    } else if (s_f) {
        flush_written_in(0);
    }
    if (s_f) {
        uint32_t st[4] = { xbox_ContiguousAllocatedBytes(), g_xbox_image_lo, g_xbox_image_hi,
                           nv2a_pb_gpu_state()->flips };
        if (memcmp(st, s_state_last, sizeof st)) {
            put32(NVRCAP_STATE2);
            put32(st[0]);
            put32(st[1]);
            put32(st[2]);
            put32(st[3]);
            memcpy(s_state_last, st, sizeof st);
        }
        {
            uint32_t hh = 0, kb = 0;
            float k = 1.0f;
            if (nv2a_hud_squeeze_get(&hh, &k))
                memcpy(&kb, &k, 4);
            else
                hh = 0;
            if (hh != s_hud_last[0] || kb != s_hud_last[1]) {
                put32(NVRCAP_HUD);
                put32(hh);
                put32(kb);
                s_hud_last[0] = hh;
                s_hud_last[1] = kb;
            }
        }
        {
            uint8_t ramp[768];
            uint32_t gen = 0;
            int on = xbox_GammaRamp(ramp, &gen);
            if (gen != s_gamma_gen) {
                int k;
                if (!on)                 /* none: the identity */
                    for (k = 0; k < 768; k++)
                        ramp[k] = (uint8_t)(k & 0xFF);
                put32(NVRCAP_GAMMA);
                fwrite(ramp, sizeof ramp, 1, s_f);
                s_bytes += sizeof ramp;
                s_gamma_gen = gen;
            }
        }
        {
            /* Every 5 s: what the capture costs so far (a run killed by a
             * signal never prints the close line). */
            uint64_t t = now_ns();
            if (!s_next_stats)
                s_next_stats = t + 5000000000ull;
            else if (t >= s_next_stats) {
                s_next_stats = t + 5000000000ull;
                fprintf(stderr, LOGP "so far: %llu kicks, %llu software methods, %llu pages"
                                " written (%llu the GPU's, not), %.0f MB; %.1f ms finding"
                                " written pages (%.1f us a scan)\n",
                        (unsigned long long)s_kicks, (unsigned long long)s_swms,
                        (unsigned long long)s_pages, (unsigned long long)s_gpu_pages,
                        s_bytes / 1048576.0, s_scan_ns / 1e6,
                        s_scan_ns / 1e3 / (double)(s_kicks + s_swms + 1));
            }
        }
        put32(NVRCAP_KICK);
        put32(from);
        put32(to);
        s_kicks++;
        /* Flushed per kick: a headless run ends on timeout's SIGTERM or the
         * watchdog's _exit, neither of which runs atexit, so the file is
         * whole up to the last kick (nvr_replay reads a file cut short). */
        fflush(s_f);
        if (s_bytes >= s_limit)
            end_file("size cap");
    }
    pthread_mutex_unlock(&s_mu);
}

void nvr_capture_swm(uint32_t param)
{
    if (s_state <= 0)
        return;
    pthread_mutex_lock(&s_mu);
    if (s_f) {
        flush_written_in(1);
        s_swms++;
        put32(NVRCAP_SWM);
        put32(param);
    }
    pthread_mutex_unlock(&s_mu);
}

void nvr_capture_gpu_wrote(uint32_t addr, uint32_t len)
{
    int i;

    if (s_state <= 0 || !len)
        return;
    pthread_mutex_lock(&s_mu);
    for (i = 0; s_f && i < s_nv; i++) {
        const xbox_mem_view *v = &s_v[i];
        uint64_t lo, hi, off;
        int sp = v->space;
        if (sp == 2 || addr < v->guest || (uint64_t)addr >= (uint64_t)v->guest + v->size)
            continue;
        lo = (addr - v->guest) & ~(uint64_t)(NVRCAP_PAGE_SIZE - 1);
        hi = (uint64_t)addr - v->guest + len;
        if (hi > v->size)
            hi = v->size;
        /* The shadow takes the page as it now stands: unchanged by the title
         * at the next scan, it is not written. */
        for (off = lo; off < hi; off += NVRCAP_PAGE_SIZE)
            memcpy(s_shadow[sp] + off, (const uint8_t *)s_prim[sp]->host + off,
                   NVRCAP_PAGE_SIZE);
        s_gpu_pages += (hi - lo + NVRCAP_PAGE_SIZE - 1) / NVRCAP_PAGE_SIZE;
        break;
    }
    pthread_mutex_unlock(&s_mu);
}

void nvr_capture_close_nowait(void)
{
    if (s_state <= 0 || pthread_mutex_trylock(&s_mu) != 0)
        return;
    end_file("exit");
    pthread_mutex_unlock(&s_mu);
}

void nvr_capture_close(void)
{
    pthread_mutex_lock(&s_mu);
    end_file("exit");
    pthread_mutex_unlock(&s_mu);
}

#else

void nvr_capture_kick(uint32_t from, uint32_t to) { (void)from; (void)to; }
void nvr_capture_close(void) {}
void nvr_capture_close_nowait(void) {}
void nvr_capture_swm(uint32_t param) { (void)param; }
void nvr_capture_gpu_wrote(uint32_t addr, uint32_t len) { (void)addr; (void)len; }

#endif

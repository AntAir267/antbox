/*
 * nvr_replay: a frame capture (RECOMP_NVR_CAPTURE, src/nvr/nvr_capture.h)
 * run through a GPU backend, headless, every presented frame written out.
 * The same capture through the old Vulkan backend and the native renderer
 * gives frames that differ only by the renderer: no timing, no input.
 *
 *   nvr_replay <capture> [options]
 *     --backend vulkan     the backend: vulkan (the old one), native (src/nvr), cpu
 *     --out DIR            write frames as DIR/fNNNNNN.bmp (none without it)
 *     --every N            only every Nth flip (default 1)
 *     --flips A-B          only flips A..B
 *     --max-flips N        stop after flip N
 *     --scale N            render.scale, over the capture's (2..4); a capture
 *                          made before NVRCAP_ENV records existed needs it
 *     --no-frames          no per-flip read-back and hash (each one waits for
 *                          the GPU): for timing and --prof
 *     --prof FILE          sample where the replay spends its CPU time (SIGPROF,
 *                          1 kHz) and write "count address" lines to FILE, for
 *                          prof.sh to name
 *     --contig-high HEX    the contiguous high-water mark, for a capture
 *                          made before NVRCAP_STATE records existed
 *
 * Prints a line per frame written: its flip, size and a hash of its pixels
 * (FNV-1a 64), and at the end the counts. Exit 0 when the file was read to
 * its end.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1             /* dladdr */
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ucontext.h>

#include "kernel.h"
#include "nv2a_backend_common.h"
#include "nv2a_pb_state.h"
#include "xbox_gamma.h"
#include "xbox_memory_layout.h"
#include "nvr/nvr_capture.h"
#ifdef XBOXRECOMP_HAVE_VULKAN
#include "nvr/nvr_backend.h"
#endif
#include "platform/recomp_env.h"

void nv2a_pb_scan(uint32_t from, uint32_t to);
extern void (*xbox_Nv2aSoftwareMethodReplay)(uint32_t param);
void xbox_ContiguousSetAllocatedBytes(uint32_t bytes);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;
const struct nv2a_pb_backend *nv2a_pb_cpu_backend(void);
#ifdef XBOXRECOMP_HAVE_VULKAN
const struct nv2a_pb_backend *nv2a_pb_vulkan_backend(void);
const uint8_t *nv2a_pb_vulkan_host_pixels(uint32_t offset, uint32_t *hw, uint32_t *hh);
#endif

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define LOGP "[nvr-replay] "

static struct {
    const char *out;
    unsigned every, lo, hi, max;
    int vulkan, native;
    int no_frames;
} O = { NULL, 1, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0 };

static struct nv2a_pb_backend s_wrap;      /* the backend, its flip seen here */
static const struct nv2a_pb_backend *s_real;
static unsigned s_flip, s_written;
static int s_stop;
static unsigned s_drift;

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

static void write_bmp(const char *path, const uint8_t *px, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hd[54];
    uint32_t y, v;
    if (!f)
        return;
    memset(hd, 0, sizeof hd);
    hd[0] = 'B'; hd[1] = 'M';
    v = 54 + w * h * 4; memcpy(hd + 2, &v, 4);
    v = 54;             memcpy(hd + 10, &v, 4);
    v = 40;             memcpy(hd + 14, &v, 4);
    memcpy(hd + 18, &w, 4);
    memcpy(hd + 22, &h, 4);
    hd[26] = 1; hd[28] = 32;
    fwrite(hd, sizeof hd, 1, f);
    for (y = h; y-- > 0;)                      /* bottom-up */
        fwrite(px + (size_t)y * w * 4, (size_t)w * 4, 1, f);
    fclose(f);
}

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--)
        h = (h ^ *p++) * 1099511628211ull;
    return h;
}

/* The frame just flipped, read back and written. */
static void frame_out(uint32_t surface)
{
    const struct nv2a_pb_present *p = nv2a_pb_present_state();
    const uint8_t *px = NULL;
    uint32_t w = 0, h = 0;
    char path[1024];

    if (O.no_frames || s_flip < O.lo || s_flip > O.hi || (s_flip - O.lo) % O.every)
        return;
#ifdef XBOXRECOMP_HAVE_VULKAN
    if (O.vulkan)
        px = nv2a_pb_vulkan_host_pixels(p->offset ? p->offset : surface, &w, &h);
    if (O.native)
        px = nvr_host_pixels(p->offset ? p->offset : surface, &w, &h);
#endif
    if (!px) {
        printf("flip %u: no pixels for surface 0x%08X\n", s_flip, p->offset);
        return;
    }
    printf("flip %u: %ux%u %016llx\n", s_flip, w, h,
           (unsigned long long)fnv64(px, (size_t)w * h * 4));
    if (O.out) {
        snprintf(path, sizeof path, "%s/f%06u.bmp", O.out, s_flip);
        write_bmp(path, px, w, h);
    }
    s_written++;
}

static void on_flip(uint32_t surface, uint32_t pitch)
{
    s_real->on_flip(surface, pitch);
    s_flip++;
    frame_out(surface);
    if (s_flip >= O.max)
        s_stop = 1;
}

static int rd(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static FILE *s_f;
static xbox_mem_view *s_prim[3];
static unsigned long long s_kicks, s_pages;
static unsigned s_swm_ok, s_swm_bad, s_env, s_env_scale;

/* The capture's settings that are the run's, not the renderer's: which
 * backend (the replay picks its own, and wraps its flips), pacing and the
 * vblank (a replay runs as fast as it can), the window, the save folder. */
static int replay_skips(const char *kv)
{
    static const char *const run_only[] = {
        "RECOMP_PB_EXEC=", "RECOMP_PB_BACKEND=", "RECOMP_FLIP_PACING=", "RECOMP_VBLANK=",
        "RECOMP_HEADLESS=", "RECOMP_SAVE_DIR=", "RECOMP_AC97_READY=",
        "RECOMP_PRESENT_FULLSCREEN=", "RECOMP_PRESENT_PACING=", "RECOMP_PRESENT_SPIN_LEAD=",
    };
    size_t i;
    for (i = 0; i < sizeof run_only / sizeof run_only[0]; i++)
        if (!strncmp(kv, run_only[i], strlen(run_only[i])))
            return 1;
    return 0;
}

/* --prof: program counters sampled at 1 kHz of CPU time, counted in an
 * open-addressed table (no allocation in the handler). */
#define PROF_SLOTS 65536
static struct { uintptr_t pc; uint32_t n; } s_prof[PROF_SLOTS];
static volatile uint32_t s_prof_total, s_prof_lost;

static void prof_sig(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    uintptr_t pc;
    uint32_t h, i;
    (void)sig; (void)si;
#if defined(__x86_64__)
    pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    pc = 0;
#endif
    s_prof_total++;
    h = (uint32_t)((pc >> 2) * 2654435761u) & (PROF_SLOTS - 1);
    for (i = 0; i < 64; i++, h = (h + 1) & (PROF_SLOTS - 1)) {
        if (s_prof[h].pc == pc) { s_prof[h].n++; return; }
        if (!s_prof[h].pc) { s_prof[h].pc = pc; s_prof[h].n = 1; return; }
    }
    s_prof_lost++;
}

static void prof_start(void)
{
    struct sigaction sa;
    struct itimerval it;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_sig;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    memset(&it, 0, sizeof it);
    it.it_interval.tv_usec = 1000;
    it.it_value.tv_usec = 1000;
    setitimer(ITIMER_PROF, &it, NULL);
}

static void prof_write(const char *path)
{
    struct itimerval it;
    FILE *f;
    int i;
    memset(&it, 0, sizeof it);
    setitimer(ITIMER_PROF, &it, NULL);
    f = fopen(path, "w");
    if (!f)
        return;
    /* Addresses as offsets into the executable, for addr2line. */
    {
        extern char __executable_start;
        fprintf(f, "# samples %u lost %u base %p\n", s_prof_total, s_prof_lost,
                (void *)&__executable_start);
        for (i = 0; i < PROF_SLOTS; i++) {
            Dl_info di;
            if (!s_prof[i].pc)
                continue;
            /* Outside the executable: the library and its nearest symbol. */
            if (dladdr((void *)s_prof[i].pc, &di) && di.dli_fbase
                    && (uintptr_t)di.dli_fbase != (uintptr_t)&__executable_start) {
                const char *lib = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
                fprintf(f, "%u lib %s:%s\n", s_prof[i].n, lib ? lib + 1 : "?",
                        di.dli_sname ? di.dli_sname : "?");
                continue;
            }
            fprintf(f, "%u %lx %lx\n", s_prof[i].n,
                    (unsigned long)(s_prof[i].pc - (uintptr_t)&__executable_start),
                    (unsigned long)s_prof[i].pc);
        }
    }
    fclose(f);
}

/* A crash prints its stack as executable offsets (addr2line -f -C -e the
 * binary), then dies as it would have. */
static void crash_sig(int sig)
{
    void *pc[48];
    char line[96];
    int n = backtrace(pc, 48), i;
    extern char __executable_start;
    for (i = 0; i < n; i++) {
        Dl_info di;
        int len;
        if (dladdr(pc[i], &di) && di.dli_fbase
                && (uintptr_t)di.dli_fbase != (uintptr_t)&__executable_start)
            len = snprintf(line, sizeof line, "[crash] #%d %s+%lx\n", i,
                           di.dli_sname ? di.dli_sname : "?",
                           (unsigned long)((uintptr_t)pc[i] - (uintptr_t)di.dli_saddr));
        else
            len = snprintf(line, sizeof line, "[crash] #%d exe 0x%lx\n", i,
                           (unsigned long)((uintptr_t)pc[i] - (uintptr_t)&__executable_start));
        if (len > 0 && write(2, line, (size_t)len) < 0)
            break;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static unsigned s_cli_scale;
static int s_opts_final;

static void host_opts(unsigned scale)
{
    struct nv2a_host_opts o;
    memset(&o, 0, sizeof o);
    /* No disk caches, so runs compare, unless a run measures them. */
    o.shader_cache = getenv("NVR_REPLAY_CACHE") ? getenv("NVR_REPLAY_CACHE") : "none";
    o.render_scale = scale;
    nv2a_host_opts_set(&o);
}

/* One record. Returns its type, -1 at the end of the file, -2 on a bad one.
 * Inside a software method (in_swm) a KICK is not run: it is put back, as
 * the capture has no record of the method that is running. */
static int step(int in_swm)
{
    static uint8_t page[NVRCAP_PAGE_SIZE];
    uint32_t t, a[3];
    uint64_t sz;
    FILE *f = s_f;

    if (!rd(f, &t, 4))
        return -1;
    if (t == NVRCAP_SPACE) {
        if (!rd(f, a, 8) || !rd(f, &sz, 8)) return -1;
        if (a[0] >= 3 || !s_prim[a[0]] || s_prim[a[0]]->size < sz) {
            fprintf(stderr, LOGP "space %u (%llu bytes at 0x%08X) does not fit here\n",
                    a[0], (unsigned long long)sz, a[1]);
            return -2;
        }
        memset(s_prim[a[0]]->host, 0, (size_t)sz);
    } else if (t == NVRCAP_PAGE) {
        if (!rd(f, a, 8) || !rd(f, page, sizeof page)) return -1;
        if (a[0] < 3 && s_prim[a[0]] && a[1] + sizeof page <= s_prim[a[0]]->size)
            memcpy((uint8_t *)s_prim[a[0]]->host + a[1], page, sizeof page);
        s_pages++;
    } else if (t == NVRCAP_STATE || t == NVRCAP_STATE2) {
        uint32_t live = 0x7FFFFFFFu;
        if (!rd(f, a, 12)) return -1;
        if (t == NVRCAP_STATE2 && !rd(f, &live, 4)) return -1;
        /* The executor's flip count as the capture saw it before this
         * kick: a replay that differs walked something else. */
        if (live != 0x7FFFFFFFu && live != nv2a_pb_gpu_state()->flips && !s_drift++)
            printf(LOGP "flip count drifts at kick %llu: live %u, replay %u\n",
                   s_kicks, live, nv2a_pb_gpu_state()->flips);
        xbox_ContiguousSetAllocatedBytes(a[0]);
        g_xbox_image_lo = a[1];
        g_xbox_image_hi = a[2];
    } else if (t == NVRCAP_ENV) {
        char kv[512], *eq;
        uint32_t n;
        int id;
        if (!rd(f, &n, 4) || n >= sizeof kv || !rd(f, kv, n)) return -1;
        kv[n] = 0;
        eq = strchr(kv, '=');
        if (eq && !replay_skips(kv)) {
            *eq = 0;
            for (id = 0; id < RENV_COUNT; id++)
                if (!strcmp(recomp_env_name((recomp_env_id)id), kv)) {
                    recomp_env_set((recomp_env_id)id, eq + 1);
                    s_env++;
                    if (id == RENV_RENDER_SCALE)
                        s_env_scale = (unsigned)atoi(eq + 1);
                    break;
                }
        }
    } else if (t == NVRCAP_HUD) {
        float k;
        if (!rd(f, a, 8)) return -1;
        memcpy(&k, &a[1], 4);
        nv2a_hud_squeeze(a[0], a[0] ? k : 1.0f);
    } else if (t == NVRCAP_GAMMA) {
        uint8_t ramp[768];
        if (!rd(f, ramp, sizeof ramp)) return -1;
        xbox_GammaSetRamp(ramp);
    } else if (t == NVRCAP_SWM) {
        if (!rd(f, a, 4)) return -1;
        if (!in_swm && s_swm_bad++ < 4)       /* one the replay didn't reach */
            printf(LOGP "kick %llu: the capture has software method %u the replay didn't run\n",
                   s_kicks, a[0]);
    } else if (t == NVRCAP_KICK) {
        if (in_swm) {
            fseek(f, -4, SEEK_CUR);
            return t;
        }
        if (!rd(f, a, 8)) return -1;
        if (!s_opts_final) {
            /* The capture's settings came before its first kick: the
             * backend starts with them (render.scale unless --scale). */
            s_opts_final = 1;
            host_opts(s_cli_scale ? s_cli_scale : s_env_scale ? s_env_scale : 1);
            if (s_env)
                printf(LOGP "%u settings from the capture; render.scale %u\n", s_env,
                       s_cli_scale ? s_cli_scale : s_env_scale ? s_env_scale : 1);
        }
        nv2a_pb_scan(a[0], a[1]);
        s_kicks++;
    } else if (t != NVRCAP_END) {
        fprintf(stderr, LOGP "unknown record %u at %ld\n", t, ftell(f));
        return -2;
    }
    return (int)t;
}

/* The executor reached a software method: what the title's dispatcher
 * wrote, from the capture, up to its NVRCAP_SWM record. */
static void swm_replay(uint32_t param)
{
    for (;;) {
        int t = step(1);
        if (t == NVRCAP_SWM) {
            s_swm_ok++;
            return;
        }
        if (t == NVRCAP_KICK || t < 0 || t == NVRCAP_END) {
            if (s_swm_bad++ < 4)
                printf(LOGP "kick %llu: software method %u not in the capture\n", s_kicks, param);
            return;
        }
    }
}

int main(int argc, char **argv)
{
    static uint8_t xbe[0x1000];
    xbox_mem_view v[48];
    const char *which = "vulkan", *path = NULL;
    char magic[8];
    FILE *f;
    xbox_mem_view **prim = s_prim;
    int i, nv, ended = 0;
    uint32_t contig_high = 0;
    unsigned scale = 0;
    const char *prof = NULL;

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGSEGV, crash_sig);
    signal(SIGBUS, crash_sig);
    signal(SIGABRT, crash_sig);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--backend") && i + 1 < argc) which = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) O.out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) O.every = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-flips") && i + 1 < argc) O.max = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prof") && i + 1 < argc) prof = argv[++i];
        else if (!strcmp(argv[i], "--no-frames")) O.no_frames = 1;
        else if (!strcmp(argv[i], "--contig-high") && i + 1 < argc)
            contig_high = (uint32_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--flips") && i + 1 < argc) {
            if (sscanf(argv[++i], "%u-%u", &O.lo, &O.hi) != 2) { O.hi = O.lo; }
        } else if (argv[i][0] != '-' && !path) path = argv[i];
        else { fprintf(stderr, "usage: nvr_replay <capture> [--backend vulkan|cpu] [--out DIR]"
                               " [--every N] [--flips A-B] [--max-flips N]\n"); return 2; }
    }
    if (!path || !O.every) { fprintf(stderr, LOGP "no capture given\n"); return 2; }
    if (O.out)
        mkdir(O.out, 0755);
    f = s_f = fopen(path, "rb");
    if (!f || !rd(f, magic, 8) || memcmp(magic, NVRCAP_MAGIC, 8)) {
        fprintf(stderr, LOGP "%s: not a capture\n", path);
        return 1;
    }
    s_cli_scale = scale;
    host_opts(scale ? scale : 1);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, LOGP "guest memory init failed\n");
        return 1;
    }
    /* The replay walks the push buffer itself: the GPU thread would walk it
     * too once a restored register page moves DMA_PUT. */
    xbox_Nv2aAckStop();
    if (contig_high)
        xbox_ContiguousSetAllocatedBytes(contig_high);
    nv = xbox_MemoryViews(v, 48);
    for (i = 0; i < nv; i++)
        if (v[i].space >= 0 && v[i].space < 3 && !prim[v[i].space])
            prim[v[i].space] = &v[i];

    if (!strcmp(which, "cpu"))
        s_real = nv2a_pb_cpu_backend();
#ifdef XBOXRECOMP_HAVE_VULKAN
    else if (!strcmp(which, "vulkan")) {
        s_real = nv2a_pb_vulkan_backend();
        O.vulkan = 1;
    }
    else if (!strcmp(which, "native")) {
        s_real = nvr_backend();
        O.native = 1;
    }
#endif
    if (!s_real) {
        fprintf(stderr, LOGP "no backend \"%s\" in this build\n", which);
        return 1;
    }
    s_wrap = *s_real;
    s_wrap.on_flip = on_flip;
    nv2a_pb_set_backend(&s_wrap);

    xbox_Nv2aSoftwareMethodReplay = swm_replay;
    if (prof)
        prof_start();
    while (!s_stop) {
        int t = step(0);
        if (t == NVRCAP_END) { ended = 1; break; }
        if (t < 0) { if (t == -2) return 1; break; }
    }
    printf(LOGP "%llu kicks, %llu pages, %u flips, %u frames written%s\n", s_kicks, s_pages,
           s_flip, s_written, ended ? "" : (s_stop ? " (stopped)" : " (file cut short)"));
    if (prof)
        prof_write(prof);
#ifdef XBOXRECOMP_HAVE_VULKAN
    if (O.native)
        nvr_print_stats();
#endif
    printf(LOGP "software methods: %u replayed from the capture", s_swm_ok);
    if (s_swm_bad)
        printf(", %u that didn't line up with it", s_swm_bad);
    printf("\n");
    if (s_drift)
        printf(LOGP "flip count differed from the capture's before %u kicks\n", s_drift);
    fclose(f);
    /* A file cut short (a run killed by a signal) still replays up to its
     * last whole kick. */
    return ended || s_stop || s_kicks ? 0 : 1;
}

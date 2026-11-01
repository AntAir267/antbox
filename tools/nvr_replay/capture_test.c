/*
 * nvr_capture_test: the frame capture's page tracking (src/nvr/nvr_capture.c)
 * against writes it must not miss. Guest memory is set up as a title has it;
 * pages are written through the base view, a RAM mirror, the contiguous
 * window, the tiled aperture over it, and by the kernel (read() into guest
 * memory); a thread keeps writing while kicks are taken. The capture file is
 * then read back and every write checked against the last copy of its page
 * before the kick that followed it. Exit 0 when nothing was missed.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void xbox_ContiguousSetAllocatedBytes(uint32_t bytes);

#include "xbox_memory_layout.h"
#include "nvr/nvr_capture.h"

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define LOGP "[nvr-capture-test] "
#define KICKS 400
#define SLOTS 64                 /* pages the writer thread cycles through */
#define SWMS 2000                /* software methods, after the kicks */

static xbox_mem_view s_v[48];
static int s_nv;
static uint8_t *s_ram, *s_contig, *s_mirror, *s_tiled;
static volatile int s_run = 1;
/* What the writer last wrote to slot i before kick k: [k][i]. */
static uint32_t s_want[KICKS + 1][SLOTS];
static volatile uint32_t s_cur[SLOTS];

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);
    v = 0;           memcpy(xbe + 0x11C, &v, 4);
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4);
}

/* Slot i: a page in the contiguous window 0x02000000 + i pages, written
 * alternately through the window and through the tiled aperture. */
static void *writer(void *arg)
{
    uint32_t n = 1;
    (void)arg;
    while (s_run) {
        int i = (int)(n % SLOTS);
        uint8_t *base = (n & 1) && s_tiled ? s_tiled : s_contig;
        volatile uint32_t *w = (volatile uint32_t *)(base + 0x02000000u + (size_t)i * 4096u);
        *w = n;
        s_cur[i] = n;
        n++;
        /* Sparse enough that a page is often clean when a kick looks: the
         * soft-dirty fallback lost a page written between its scan and its
         * clear, which a page written all the time never shows. */
        usleep(5);
    }
    return NULL;
}

int main(void)
{
    static uint8_t xbe[0x1000];
    char path[256];
    pthread_t th;
    int i, k, fails = 0, pipefd[2], swm_slow = 0, swm_seen = 0;
    FILE *f;

    snprintf(path, sizeof path, "/tmp/nvr_capture_test_%d.nvrcap", (int)getpid());
    setenv("RECOMP_NVR_CAPTURE", path, 1);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe))
        return 1;
    xbox_Nv2aAckStop();
    s_nv = xbox_MemoryViews(s_v, 48);
    for (i = 0; i < s_nv; i++) {
        if (s_v[i].space == 0 && !s_ram) s_ram = (uint8_t *)s_v[i].host;
        else if (s_v[i].space == 0 && !s_mirror) s_mirror = (uint8_t *)s_v[i].host;
        else if (s_v[i].space == 1 && !s_contig) s_contig = (uint8_t *)s_v[i].host;
        else if (s_v[i].space == 1 && !s_tiled) s_tiled = (uint8_t *)s_v[i].host;
    }
    if (!s_ram || !s_contig) {
        fprintf(stderr, LOGP "FAIL: no RAM or contiguous view\n");
        return 1;
    }
    s_ram[0x100000] = 0x5A;                       /* before the first kick */
    nvr_capture_kick(0x80000000u, 0x80000000u);    /* the snapshot */
    if (s_mirror)
        s_mirror[0x200000] = 0xA5;                 /* RAM through a mirror */
    if (pipe(pipefd) == 0 && write(pipefd[1], "kern", 4) == 4
            && read(pipefd[0], s_contig + 0x300000, 4) != 4)
        fprintf(stderr, LOGP "read() into guest memory failed\n");
    pthread_create(&th, NULL, writer, NULL);
    for (k = 1; k <= KICKS; k++) {
        usleep(200);
        /* The values written before this kick is taken; later ones may or
         * may not make it, so only these are required. */
        for (i = 0; i < SLOTS; i++)
            s_want[k][i] = s_cur[i];
        nvr_capture_kick(0x80000000u, 0x80000000u);
    }
    s_run = 0;
    pthread_join(th, NULL);
    /* Software methods: what the dispatcher writes in the contiguous window
     * must be in the file before the method's record (the walk reads it
     * next), and each must be cheap (a Wreckless mission runs 4-5k a
     * second). The writes go below a 32 MB high-water mark, as allocated. */
    {
        struct timespec t0, t1;
        double us;
        xbox_ContiguousSetAllocatedBytes(32u << 20);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (k = 0; k < SWMS; k++) {
            uint32_t v = 0x5000u + (uint32_t)k;
            memcpy(s_contig + 0x01000000u + (size_t)(k % 16) * 4096u, &v, 4);
            nvr_capture_swm((uint32_t)k);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3 / SWMS;
        printf(LOGP "%d software methods, %.1f us each\n", SWMS, us);
        if (us > 60.0) {
            fprintf(stderr, LOGP "FAIL: a software method's scan is too slow\n");
            swm_slow = 1;
        }
    }
    nvr_capture_close();

    /* Read it back: the guest as the replay would rebuild it, checked at
     * each kick. */
    f = fopen(path, "rb");
    {
        static uint8_t ram[64u << 20], contig[64u << 20];
        uint8_t page[4096];
        char magic[8];
        uint32_t t, a[4];
        int kick = 0;
        if (!f || fread(magic, 8, 1, f) != 1)
            return 1;
        while (fread(&t, 4, 1, f) == 1 && t != NVRCAP_END) {
            if (t == NVRCAP_SPACE) { uint64_t sz; if (fread(a, 8, 1, f) != 1 || fread(&sz, 8, 1, f) != 1) break; }
            else if (t == NVRCAP_PAGE) {
                if (fread(a, 8, 1, f) != 1 || fread(page, 4096, 1, f) != 1) break;
                if (a[0] == 0 && a[1] + 4096 <= sizeof ram) memcpy(ram + a[1], page, 4096);
                if (a[0] == 1 && a[1] + 4096 <= sizeof contig) memcpy(contig + a[1], page, 4096);
            } else if (t == NVRCAP_STATE) { if (fread(a, 12, 1, f) != 1) break; }
            else if (t == NVRCAP_STATE2) { if (fread(a, 16, 1, f) != 1) break; }
            else if (t == NVRCAP_GAMMA) { if (fread(page, 768, 1, f) != 1) break; }
            else if (t == NVRCAP_ENV) {
                uint32_t n;
                if (fread(&n, 4, 1, f) != 1 || n > sizeof page || fread(page, n, 1, f) != 1) break;
            }
            else if (t == NVRCAP_HUD) { if (fread(a, 8, 1, f) != 1) break; }
            else if (t == NVRCAP_SWM) {
                uint32_t v;
                if (fread(a, 4, 1, f) != 1) break;
                memcpy(&v, contig + 0x01000000u + (size_t)(a[0] % 16) * 4096u, 4);
                if (v != 0x5000u + a[0]) {
                    if (fails < 10)
                        fprintf(stderr, LOGP "FAIL: software method %u: its write is not"
                                " before its record (%#x)\n", a[0], v);
                    fails++;
                }
                swm_seen++;
            }
            else if (t == NVRCAP_KICK) {
                if (fread(a, 8, 1, f) != 1) break;
                if (kick == 0 && ram[0x100000] != 0x5A) {
                    fprintf(stderr, LOGP "FAIL: the snapshot misses a RAM write\n");
                    fails++;
                }
                if (kick == 1) {
                    if (s_mirror && ram[0x200000] != 0xA5) {
                        fprintf(stderr, LOGP "FAIL: a write through a RAM mirror\n");
                        fails++;
                    }
                    if (memcmp(contig + 0x300000, "kern", 4)) {
                        fprintf(stderr, LOGP "FAIL: the kernel's write (read())\n");
                        fails++;
                    }
                }
                if (kick >= 1 && kick <= KICKS)
                    for (i = 0; i < SLOTS; i++) {
                        uint32_t got;
                        memcpy(&got, contig + 0x02000000u + (size_t)i * 4096u, 4);
                        if (got < s_want[kick][i]) {
                            if (fails < 10)
                                fprintf(stderr, LOGP "FAIL: kick %d slot %d has %u, was %u\n",
                                        kick, i, got, s_want[kick][i]);
                            fails++;
                        }
                    }
                kick++;
            } else {
                fprintf(stderr, LOGP "FAIL: record %u\n", t);
                return 1;
            }
        }
        fclose(f);
        if (swm_seen != SWMS) {
            fprintf(stderr, LOGP "FAIL: %d software-method records, want %d\n", swm_seen, SWMS);
            fails++;
        }
        printf(LOGP "%d kicks read, %d missed writes: %s\n", kick, fails,
               fails || swm_slow ? "FAIL" : "PASS");
    }
    unlink(path);
    return fails || swm_slow ? 1 : 0;
}

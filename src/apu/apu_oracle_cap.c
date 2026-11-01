/*
 * apu_oracle_cap.c -- RECOMP_ORACLE_CAP=<file>: an opt-in dump of what the
 * title hands the audio DSP stage, for measurements made outside this
 * project. Off by default; nothing in the mix reads it, and with the
 * variable unset each hook is one branch.
 *
 * This file and its hooks contain no xemu-derived code: they dump this
 * project's own buffers and values. Order of writing, for the record: the
 * MMIO, SCRF/SCRD, DBEL, BINS, PMAP, OUT6 and DSPF/DSPD records were written
 * before their author had opened xemu's DSP source; the VRET record was
 * added afterwards (it dumps this build's own voice values).
 *
 * Plain buffers only, as records of {u32 tag, u32 frame, u32 bytes, payload}:
 *
 *   MMIO  addr val size        a write to the APU's registers 0x2000..0x4FFF
 *                              or to the GP / EP regions (0x30000 and up)
 *   MMRD  addr size            a read of the GP / EP regions
 *   SCRF  gpsaddr npages, npages page addresses, npages * 4096 bytes
 *                              the GP scratch memory, whole (first seen, or
 *                              its table changed)
 *   SCRD  (offset value)...    the scratch dwords that changed since the
 *                              last look
 *   DSPF  base bytes, the bytes
 *                              the GP / EP DSP memory windows of the APU
 *                              (0x30000..0x7FFFF, plain RAM in this build),
 *                              whole, the first time they are looked at
 *   DSPD  (offset value)...    their dwords that changed since (offsets
 *                              from the APU's base)
 *   DBEL  value                the command word the doorbell stand-in is
 *                              about to clear (apu_dsp.c); the scratch is
 *                              looked at first, so the command block is in
 *                              the SCRD before it
 *   VRET  voice fmt buffer ebo cbo, 8 x (bin | vol << 8 | lowpass << 24),
 *         8 float gains        a voice playing 32-bit containers, each frame
 *                              it is mixed: its buffer's physical address,
 *                              end and current offsets, and its gain into
 *                              each of its eight bins (envelope * volume /
 *                              headroom). Tagged with the frame before the
 *                              BINS record it is part of
 *   BINS  mask, 32 floats for each set bit
 *                              the mixbins as the voice processor leaves
 *                              them, one record a 32-sample frame
 *   PMAP  32 int8, 32 floats   this build's bin -> speaker map and bin gains
 *   OUT6  32 * 6 floats        this build's six speaker sums of the frame,
 *                              before the clamp (empty: silent)
 *
 * "frame" counts the frames the DSP stage has been given (BINS records).
 */
#include "apu_state.h"
#include "apu_speakers.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCAP_PAGE 4096u
#define OCAP_MAX_PAGES 4096u

static int s_on = -1;
static FILE *s_f;
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_frame;

static uint32_t s_sge, s_npages;
static uint32_t *s_page_addr;
static uint8_t *s_shadow;

int apu_ocap_on(void)
{
    if (s_on < 0) {
        const char *e = getenv("RECOMP_ORACLE_CAP");
        s_f = e && *e && *e != '0' ? fopen(e, "wb") : NULL;
        if (s_f)
            setvbuf(s_f, NULL, _IOFBF, 1 << 20);
        if (e && *e && *e != '0')
            fprintf(stderr, "[APU] oracle capture -> %s%s\n", e, s_f ? "" : " (open failed)");
        s_on = s_f != NULL;
    }
    return s_on;
}

static void rec(const char tag[4], const void *a, uint32_t an, const void *b, uint32_t bn)
{
    uint32_t h[3];
    memcpy(&h[0], tag, 4);
    h[1] = s_frame;
    h[2] = an + bn;
    pthread_mutex_lock(&s_mu);
    fwrite(h, 4, 3, s_f);
    if (an) fwrite(a, 1, an, s_f);
    if (bn) fwrite(b, 1, bn, s_f);
    pthread_mutex_unlock(&s_mu);
}

void apu_ocap_mmio(uint32_t addr, uint32_t val, unsigned size, int is_write)
{
    if (!apu_ocap_on())
        return;
    if (is_write) {
        uint32_t p[3] = { addr, val, size };
        if (addr >= 0x30000 || (addr >= 0x2000 && addr < 0x5000))
            rec("MMIO", p, sizeof(p), NULL, 0);
    } else if (addr >= 0x30000) {
        uint32_t p[2] = { addr, size };
        rec("MMRD", p, sizeof(p), NULL, 0);
    }
}

/* The GP scratch memory through its scatter-gather table (8 bytes an entry,
 * the page's physical address first). */
void apu_ocap_scratch(MCPXAPUState *d)
{
    uint32_t sge, np;
    int whole = 0;

    if (!apu_ocap_on() || !d->ram_ptr)
        return;
    sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]);
    np = qatomic_read(&d->regs[NV_PAPU_GPSMAXSGE]) + 1;
    if (!sge || np > OCAP_MAX_PAGES)
        return;
    if (sge != s_sge || np != s_npages) {
        free(s_page_addr);
        free(s_shadow);
        s_page_addr = (uint32_t *)calloc(np, sizeof(uint32_t));
        s_shadow = (uint8_t *)calloc(np, OCAP_PAGE);
        s_sge = sge;
        s_npages = np;
        whole = 1;
    }
    if (!s_page_addr || !s_shadow)
        return;
    for (uint32_t p = 0; p < np; p++) {
        uint32_t a = *(const uint32_t *)mcpx_apu_phys(sge + p * 8);
        if (a != s_page_addr[p]) {
            s_page_addr[p] = a;
            whole = 1;
        }
    }
    if (whole) {
        uint32_t h[2] = { sge, np };
        for (uint32_t p = 0; p < np; p++) {
            uint32_t a = s_page_addr[p];
            if (a && !(a & 0xFFF))
                memcpy(s_shadow + p * OCAP_PAGE, mcpx_apu_phys(a), OCAP_PAGE);
            else
                memset(s_shadow + p * OCAP_PAGE, 0, OCAP_PAGE);
        }
        {
            uint32_t hh[3];
            memcpy(&hh[0], "SCRF", 4);
            hh[1] = s_frame;
            hh[2] = (uint32_t)sizeof(h) + np * 4 + np * OCAP_PAGE;
            pthread_mutex_lock(&s_mu);
            fwrite(hh, 4, 3, s_f);
            fwrite(h, 4, 2, s_f);
            fwrite(s_page_addr, 4, np, s_f);
            fwrite(s_shadow, OCAP_PAGE, np, s_f);
            pthread_mutex_unlock(&s_mu);
        }
        return;
    }
    {
        static uint32_t diff[2 * 4096];
        uint32_t n = 0;
        for (uint32_t p = 0; p < np; p++) {
            uint32_t a = s_page_addr[p];
            const uint8_t *now;
            uint8_t *old = s_shadow + p * OCAP_PAGE;
            if (!a || (a & 0xFFF))
                continue;
            now = mcpx_apu_phys(a);
            if (!memcmp(now, old, OCAP_PAGE))
                continue;
            for (uint32_t o = 0; o < OCAP_PAGE; o += 4) {
                uint32_t v;
                memcpy(&v, now + o, 4);
                if (!memcmp(&v, old + o, 4))
                    continue;
                memcpy(old + o, &v, 4);
                diff[n++] = p * OCAP_PAGE + o;
                diff[n++] = v;
                if (n == 2 * 4096) {
                    rec("SCRD", diff, n * 4, NULL, 0);
                    n = 0;
                }
            }
        }
        if (n)
            rec("SCRD", diff, n * 4, NULL, 0);
    }
}

/* The DSP memory windows: what the title wrote there. */
#define OCAP_DSP_BASE 0x30000u
#define OCAP_DSP_BYTES 0x50000u
extern void *xbox_McpxWindow(void);

static void apu_ocap_dspmem(void)
{
    static uint8_t *shadow;
    static uint32_t diff[2 * 4096];
    const uint8_t *now = (const uint8_t *)xbox_McpxWindow();
    uint32_t n = 0;

    if (!now)
        return;
    now += OCAP_DSP_BASE;
    if (!shadow) {
        uint32_t h[2] = { OCAP_DSP_BASE, OCAP_DSP_BYTES };
        shadow = (uint8_t *)malloc(OCAP_DSP_BYTES);
        if (!shadow)
            return;
        memcpy(shadow, now, OCAP_DSP_BYTES);
        rec("DSPF", h, sizeof(h), shadow, OCAP_DSP_BYTES);
        return;
    }
    for (uint32_t p = 0; p < OCAP_DSP_BYTES; p += OCAP_PAGE) {
        if (!memcmp(now + p, shadow + p, OCAP_PAGE))
            continue;
        for (uint32_t o = p; o < p + OCAP_PAGE; o += 4) {
            uint32_t v;
            memcpy(&v, now + o, 4);
            if (!memcmp(&v, shadow + o, 4))
                continue;
            memcpy(shadow + o, &v, 4);
            diff[n++] = OCAP_DSP_BASE + o;
            diff[n++] = v;
            if (n == 2 * 4096) {
                rec("DSPD", diff, n * 4, NULL, 0);
                n = 0;
            }
        }
    }
    if (n)
        rec("DSPD", diff, n * 4, NULL, 0);
}

void apu_ocap_doorbell(MCPXAPUState *d, uint32_t value)
{
    if (!apu_ocap_on())
        return;
    apu_ocap_dspmem();
    apu_ocap_scratch(d);
    rec("DBEL", &value, 4, NULL, 0);
}

void apu_ocap_voice(const void *a, uint32_t an, const void *b, uint32_t bn)
{
    rec("VRET", a, an, b, bn);
}

void apu_ocap_bins(MCPXAPUState *d, const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    static float buf[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    uint32_t mask = 0, n = 0;

    if (!apu_ocap_on())
        return;
    s_frame++;
    apu_ocap_dspmem();
    apu_ocap_scratch(d);
    for (int b = 0; b < NUM_MIXBINS; b++) {
        int any = 0;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            if (mixbins[b][i] != 0.0f) { any = 1; break; }
        if (!any)
            continue;
        mask |= 1u << b;
        memcpy(buf[n++], mixbins[b], sizeof(buf[0]));
    }
    rec("BINS", &mask, 4, buf, n * (uint32_t)sizeof(buf[0]));
    if (!(s_frame % 1500))
        fflush(s_f);
}

void apu_ocap_out6(const float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME],
                   const int8_t map[NUM_MIXBINS], const float *gain)
{
    struct pmap { int8_t map[NUM_MIXBINS]; float gain[NUM_MIXBINS]; };
    static struct pmap last;
    static int have;
    struct pmap now;
    float acc[NUM_SAMPLES_PER_FRAME * APU_SPK_CHANNELS];
    int any = 0;

    if (!apu_ocap_on())
        return;
    memcpy(now.map, map, sizeof(now.map));
    for (int b = 0; b < NUM_MIXBINS; b++)
        now.gain[b] = gain ? gain[b] : 1.0f;
    if (!have || memcmp(&now, &last, sizeof(now))) {
        last = now;
        have = 1;
        rec("PMAP", now.map, sizeof(now.map), now.gain, sizeof(now.gain));
    }
    memset(acc, 0, sizeof(acc));
    for (int b = 0; b < NUM_MIXBINS; b++) {
        int sp = map[b];
        float g = now.gain[b];
        if (sp < 0 || sp >= APU_SPK_CHANNELS)
            continue;
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            float x = g * mixbins[b][i];
            if (x != 0.0f) any = 1;
            acc[i * APU_SPK_CHANNELS + sp] += x;
        }
    }
    rec("OUT6", acc, any ? (uint32_t)sizeof(acc) : 0, NULL, 0);
}

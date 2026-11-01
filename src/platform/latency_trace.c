/*
 * latency_trace.c - see latency_trace.h.
 */
#include "latency_trace.h"
#include "host_time.h"
#include "recomp_env.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define LAT_LOCK()   AcquireSRWLockExclusive(&s_lock)
#define LAT_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LAT_LOCK()   pthread_mutex_lock(&s_lock)
#define LAT_UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

#define LAT_PADS 4

static volatile int s_on = -1;
static FILE        *s_f;
static uint8_t      s_last[LAT_PADS][20];
static int          s_have[LAT_PADS];
static volatile int s_isr_pending;   /* bit per pad: changed, ISR not yet run */

int xbox_LatOn(void)
{
    if (s_on < 0) {
        LAT_LOCK();
        if (s_on < 0) {
            const char *p = recomp_env(RENV_LATENCY_TRACE);
            if (p && *p && strcmp(p, "1") && strcmp(p, "0")) {
                s_f = fopen(p, "w");
                if (s_f) {
                    setvbuf(s_f, NULL, _IOFBF, 1 << 16);
                    fprintf(s_f, "kind,n,t_ns,a,b\n");
                    fprintf(stderr, "  [LAT] latency trace to %s\n", p);
                } else {
                    fprintf(stderr, "  [LAT] cannot open %s\n", p);
                }
            } else if (p && *p && strcmp(p, "0")) {
                fprintf(stderr, "  [LAT] RECOMP_TRACE=latency needs =<file>\n");
            }
            s_on = s_f != NULL;
        }
        LAT_UNLOCK();
    }
    return s_on;
}

void xbox_LatRow(char kind, uint32_t n, uint64_t t_ns, const char *extra)
{
    if (!xbox_LatOn())
        return;
    if (!t_ns)
        t_ns = xbox_HostNowNs();
    LAT_LOCK();
    fprintf(s_f, "%c,%u,%llu,%s\n", kind, n, (unsigned long long)t_ns,
            extra ? extra : "");
    /* Flushed every few rows' worth by the buffer, and at the flip rows so a
     * killed run keeps nearly everything. */
    if (kind == 'D' || kind == 'I')
        fflush(s_f);
    LAT_UNLOCK();
}

void xbox_LatPadReport(int pad, const uint8_t *rep, int n)
{
    char hex[64];
    int i;

    if (!xbox_LatOn() || pad < 0 || pad >= LAT_PADS || n < 20)
        return;
    /* Bytes 2..19: buttons, analog buttons, sticks. */
    if (s_have[pad] && !memcmp(s_last[pad] + 2, rep + 2, 18))
        return;
    memcpy(s_last[pad], rep, 20);
    s_have[pad] = 1;
    for (i = 0; i < 18; i++)
        snprintf(hex + i * 2, 3, "%02x", rep[2 + i]);
    LAT_LOCK();
    s_isr_pending |= 1 << pad;
    LAT_UNLOCK();
    xbox_LatRow('I', (uint32_t)pad, 0, hex);
}

void xbox_LatGuestIsr(void)
{
    int bits, pad;

    if (!s_on || !s_isr_pending)
        return;
    LAT_LOCK();
    bits = s_isr_pending;
    s_isr_pending = 0;
    LAT_UNLOCK();
    for (pad = 0; pad < LAT_PADS; pad++)
        if (bits & (1 << pad))
            xbox_LatRow('R', (uint32_t)pad, 0, NULL);
}

void xbox_LatSignature(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch,
                       uint32_t bpp, char *out, int outn)
{
    enum { GX = 16, GY = 12 };
    uint32_t sum[GX * GY], cnt[GX * GY];
    uint32_t x, y;
    int i;

    if (outn < GX * GY * 2 + 1 || !w || !h || (bpp != 2 && bpp != 4)) {
        if (outn > 0)
            out[0] = 0;
        return;
    }
    memset(sum, 0, sizeof sum);
    memset(cnt, 0, sizeof cnt);
    /* Every 4th pixel of every 4th row: plenty for "did this change". */
    for (y = 0; y < h; y += 4) {
        const uint8_t *r = px + (size_t)y * pitch;
        uint32_t cy = y * GY / h;
        for (x = 0; x < w; x += 4) {
            uint32_t v, c = cy * GX + x * GX / w;
            if (bpp == 4) {
                v = ((uint32_t)r[x * 4] + r[x * 4 + 1] + r[x * 4 + 2]) / 3u;
            } else {
                uint32_t p = (uint32_t)r[x * 2] | ((uint32_t)r[x * 2 + 1] << 8);
                v = (((p & 0x1F) << 3) + (((p >> 5) & 0x3F) << 2)
                     + (((p >> 11) & 0x1F) << 3)) / 3u;
            }
            sum[c] += v;
            cnt[c]++;
        }
    }
    for (i = 0; i < GX * GY; i++)
        snprintf(out + i * 2, 3, "%02x", cnt[i] ? sum[i] / cnt[i] : 0u);
}

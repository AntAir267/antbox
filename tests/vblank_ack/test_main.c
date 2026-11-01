/*
 * The vblank acknowledgement waits for a late DPC.
 *
 * An XDK vblank DPC acks with PCRTC_INTR_0 = 1 and then spins at DISPATCH
 * until PMC_INTR_0's PCRTC bit is clear. Against plain RAM the vblank-ack
 * thread is what clears it. It used to watch for 2 ms only; a DPC the
 * one-CPU gate held off past that window spun until the nv2a-ack thread's
 * next pass, hundreds of milliseconds under a heavy push buffer, and the
 * timer thread (the vblank clock) spun with it.
 *
 * Driven here against plain words, with a second thread playing the DPC:
 * an ack inside the window, one 50 ms late (cleared within a few ms of the
 * write, not left), a re-arm before any ack (handed over to the next vblank
 * at once), the bit taken down by someone else (the watch ends), an ack more
 * than a second late (still taken), a vblank the ISR declined and the DPC
 * acks later, and the timer thread's own raising of the bits (not an ack).
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define PCRTC_VBLANK 1u
#define PMC_PCRTC    (1u << 24)

static volatile uint32_t pcrtc, pmc;
static volatile LONG tick_busy;
static HANDLE rearm;
static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static long long now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)(c.QuadPart * 1000 / f.QuadPart);
}

/* The DPC: after `delay` ms, ack and record when the PMC bit came down.
 * Other modes play someone else: 1 the next vblank's re-arm, 2 a backstop
 * clearing PMC, 3 the timer thread raising the next vblank. */
typedef struct { DWORD delay; int mode; long long acked_at, cleared_at; } Dpc;

/* What the DPC waits for its bit to clear: past the watch's 1 s phase. */
enum { DPC_SPIN_MS = 3000 };

static DWORD WINAPI dpc_thread(LPVOID p)
{
    Dpc *d = (Dpc *)p;
    long long give_up;

    Sleep(d->delay);
    if (d->mode == 1) {
        SetEvent(rearm);
        return 0;
    }
    if (d->mode == 2) {
        pmc &= ~PMC_PCRTC;
        return 0;
    }
    if (d->mode == 3) {
        /* The timer thread raising the next vblank into a live watch, with
         * a slow ISR; acked_at says whether the bits survived it. */
        Sleep(20);
        tick_busy = 1;
        pcrtc |= PCRTC_VBLANK;
        pmc |= PMC_PCRTC;
        Sleep(30);
        d->acked_at = (pcrtc & PCRTC_VBLANK) && (pmc & PMC_PCRTC);
        pcrtc &= ~PCRTC_VBLANK;
        tick_busy = 0;
        SetEvent(rearm);
        return 0;
    }
    d->acked_at = now_ms();
    pcrtc |= PCRTC_VBLANK;
    give_up = d->acked_at + DPC_SPIN_MS;
    while ((pmc & PMC_PCRTC) && now_ms() < give_up)
        SwitchToThread();
    d->cleared_at = now_ms();
    return 0;
}

static int run(Dpc *d, long *late_ms)
{
    HANDLE t;
    int r;

    pcrtc = 0;
    pmc = PMC_PCRTC;                 /* the ISR left it up for the DPC */
    t = CreateThread(NULL, 0, dpc_thread, d, 0, NULL);
    r = xbox_VblankAckWait(&pcrtc, &pmc, &tick_busy, rearm, late_ms);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return r;
}

int main(void)
{
    long late;
    int r;

    setvbuf(stdout, NULL, _IONBF, 0);
    rearm = CreateEventW(NULL, FALSE, FALSE, NULL);
    printf("vblank_ack: running\n");

    {
        Dpc d = { 0, 0, 0, 0 };
        r = run(&d, &late);
        check(r == XBOX_VBL_ACK_ON_TIME || r == XBOX_VBL_ACK_LATE,
              "prompt ack is taken");
        check(!(pmc & PMC_PCRTC) && !(pcrtc & PCRTC_VBLANK),
              "prompt ack clears PMC and PCRTC");
    }
    {
        Dpc d = { 50, 0, 0, 0 };
        r = run(&d, &late);
        check(r == XBOX_VBL_ACK_LATE, "50 ms late ack is still taken");
        check(late >= 40 && late < 500, "late_ms reports the lateness");
        check(!(pmc & PMC_PCRTC), "late ack clears PMC");
        /* The point of the change: the DPC's spin after its ack is a wait
         * step, not a pass of the nv2a-ack thread. Generous for CI hosts. */
        check(d.cleared_at - d.acked_at <= 20, "late DPC spins at most a few ms");
    }
    {
        Dpc d = { 20, 1, 0, 0 };
        r = run(&d, &late);
        check(r == XBOX_VBL_ACK_REARMED, "re-arm before an ack hands over");
    }
    {
        /* The nv2a-ack backstop (or a title acking some other way) took the
         * PMC bit down: nobody is left spinning on it. */
        Dpc d = { 30, 2, 0, 0 };
        HANDLE t;
        long long t0;

        pcrtc = 0;
        pmc = PMC_PCRTC;
        t0 = now_ms();
        t = CreateThread(NULL, 0, dpc_thread, &d, 0, NULL);
        r = xbox_VblankAckWait(&pcrtc, &pmc, &tick_busy, rearm, &late);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        check(r == XBOX_VBL_ACK_NONE, "PMC bit taken down elsewhere ends the watch");
        check(now_ms() - t0 < 500, "and promptly");
    }
    {
        /* A DPC held off past the old one-second give-up: the watch goes on
         * at its slow step and still lets it go. */
        Dpc d = { 1300, 0, 0, 0 };
        r = run(&d, &late);
        check(r == XBOX_VBL_ACK_LATE, "ack after a second is still taken");
        check(late >= 1250, "and reported that late");
        check(!(pmc & PMC_PCRTC) && !(pcrtc & PCRTC_VBLANK),
              "and clears PMC and PCRTC");
        check(d.cleared_at - d.acked_at <= 60, "the DPC spins one slow step at most");
    }
    {
        /* The declined vblank, as the timer thread runs it: bits up, ISR
         * returns without claiming (the DPC has the interrupt masked), the
         * watch is armed anyway -- PCRTC down, PMC left -- and the DPC acks
         * when the gate lets it run. */
        Dpc d = { 80, 0, 0, 0 };
        HANDLE t;

        tick_busy = 1;
        pcrtc |= PCRTC_VBLANK;
        pmc |= PMC_PCRTC;
        /* kernel_raise_interrupt: declined, nothing written */
        pcrtc &= ~PCRTC_VBLANK;      /* kernel_vblank_arm_ack */
        tick_busy = 0;
        t = CreateThread(NULL, 0, dpc_thread, &d, 0, NULL);
        r = xbox_VblankAckWait(&pcrtc, &pmc, &tick_busy, rearm, &late);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        check(r == XBOX_VBL_ACK_LATE, "declined vblank: the later ack is taken");
        check(!(pmc & PMC_PCRTC), "declined vblank: PMC cleared on the ack");
        check(d.cleared_at - d.acked_at <= 20, "declined vblank: DPC let go at once");
    }
    {
        /* A watch still running from a vblank whose DPC is late, when the
         * next one is raised: the timer thread sets PCRTC before calling
         * the ISR. That is the new vblank, not the DPC's ack, and must stay
         * up for the ISR, with PMC, until the re-arm. */
        Dpc d = { 0, 3, 0, 0 };
        HANDLE t;

        pcrtc = 0;
        pmc = PMC_PCRTC;
        tick_busy = 0;
        t = CreateThread(NULL, 0, dpc_thread, &d, 0, NULL);
        r = xbox_VblankAckWait(&pcrtc, &pmc, &tick_busy, rearm, &late);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        check(d.acked_at == 1, "tick: bits stayed up while the ISR ran");
        check(r == XBOX_VBL_ACK_REARMED, "tick: the watch went to the next vblank");
        check((pmc & PMC_PCRTC) != 0, "tick: PMC left for the DPC");
        tick_busy = 0;
    }

    printf("vblank_ack: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}

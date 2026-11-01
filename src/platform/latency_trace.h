/*
 * latency_trace.h - RECOMP_TRACE=latency=<file>: input-to-photon stamps.
 *
 * One CSV row per event, every row on the one host clock (xbox_HostNowNs),
 * so tools/perf/latency.py can follow a pad change through the pipeline to
 * the frame that shows it (BO-74):
 *
 *   I,<pad>,<t>,<report hex>     a pad report differs from the last one
 *                                (usb_gamepad.c: the moment the guest's
 *                                interrupt transfer is filled)
 *   R,<pad>,<t>,                 the OHCI ISR ran after that report: the
 *                                guest's driver has it
 *   S,<flip>,<t>,<interval>      the swap's software method reached the
 *                                executor (the guest's D3D Swap, as the GPU
 *                                front end sees it)
 *   V,<flip>,<t>,                FLIP_STALL released (the emulated vblank,
 *                                with RECOMP_FLIP_PACING=vblank)
 *   E,<flip>,<t>,                the backend's on_flip returned: the frame is
 *                                committed to the host GPU and handed off
 *   G,<flip>,<t>,<gpu_end>,<sig> the frame's GPU work completed (Metal
 *                                completion handler; gpu_end from the command
 *                                buffer's GPUEndTime), sig a 16x12 grid of
 *                                cell means of the frame, hex bytes
 *   W,<flip>,<t>,<acquire t>     the presenter picked the frame up (main
 *                                thread awake) and, second field, got a
 *                                drawable (nextDrawable returned)
 *   P,<flip>,<t>,                the present was committed (presentDrawable)
 *   D,<flip>,<t>,<presentedTime> the drawable was on glass (Metal
 *                                presentedHandler; 0 when the system dropped it)
 *
 * <t> is nanoseconds on xbox_HostNowNs. Off unless set; every entry point
 * is one load and a branch then.
 */
#ifndef XBOX_LATENCY_TRACE_H
#define XBOX_LATENCY_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  xbox_LatOn(void);
/* A row: kind, a number (pad or flip), a time, and free text (may be NULL). */
void xbox_LatRow(char kind, uint32_t n, uint64_t t_ns, const char *extra);
/* usb_gamepad_report's finished report: an I row when it changed. */
void xbox_LatPadReport(int pad, const uint8_t *rep, int n);
/* The guest's OHCI ISR has run: an R row for each pad changed since. */
void xbox_LatGuestIsr(void);
/* The 16x12 signature of a frame, from w x h pixels of bpp bytes. */
void xbox_LatSignature(const uint8_t *px, uint32_t w, uint32_t h, uint32_t pitch,
                       uint32_t bpp, char *out, int outn);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_LATENCY_TRACE_H */

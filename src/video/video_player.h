/**
 * Shared FMV player: Media Foundation decode onto a D3D8 texture.
 *
 * For a title whose video is a container Windows can already decode, the
 * decoder does not have to be emulated for the video to be watchable. The
 * recompiled title still decides when: it opens the file as its own logic
 * dictates, and the runtime plays the file it opened.
 */

#ifndef XBOXRECOMP_VIDEO_PLAYER_H
#define XBOXRECOMP_VIDEO_PLAYER_H

#include <stddef.h>   /* wchar_t */
#include <stdint.h>

/* Initialize/shutdown Media Foundation (call once at app start/end) */
int  video_init(void);
void video_shutdown(void);

/* Open a video file for playback. Returns 0 on success. */
int  video_open(const char *path);

/* Advance playback by dt seconds, decode next frame if needed.
 * Returns 1 if a new frame is ready, 0 if unchanged, -1 if finished. */
int  video_update(float dt);

/* Render the current video frame as a fullscreen quad.
 * Call between BeginScene/EndScene. */
void video_render(void);

/* Check if the video has finished playing. */
int  video_is_finished(void);

/* Close the current video and release resources. */
void video_close(void);

/* === Boot sequence state machine === */

/* Boot phases */

/* Get current boot phase */

/* Advance boot state machine. Call once per frame.
 * skip=1 if user pressed a button to skip current video.
 * Returns the new phase. */

/* Render current boot phase (video frame or press-start screen). */


/* Play one video file, start to finish, in its own window on its own thread.
 * Returns 0 if the pump started. Blocking work happens on that thread, so the
 * guest carries on running while the video plays. */
int  xbox_VideoPlayFile(const char *host_path);
int  xbox_VideoIsPlaying(void);

/* Write the current decoded frame to a 24-bit BMP -- evidence that real
 * pixels reached the renderer, which a log line cannot give. */
int  video_dump_frame_bmp(const char *path);

/* Show the guest framebuffer in its own window (RECOMP_FB_WINDOW). Whatever
 * the title renders into guest RAM appears there; nothing else scans it out. */
void xbox_FramebufferWindowStart(void);
void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
int  xbox_FramebufferDumpBmp(const char *path);
/* Title bar: the game's name (window_title.h). The name is the certificate's
 * UTF-16 title (40 chars max), unless the title set its own with
 * xbox_HostWindowSetTitle. RECOMP_TRACE=title appends " | FPS: n | draws: n",
 * refreshed once a second from each flip's stats. */
void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);
void xbox_FramebufferWindowFrameStats(uint32_t draws);
#if defined(_WIN32) && !defined(XBOXRECOMP_WIN_VULKAN)
/* That title bar text, for a Win32 window that keeps it current (the D3D11
 * backend's): fills tb (n wide chars) and returns 1 when the window should
 * write it -- when the name changes, or once a second with
 * RECOMP_TRACE=title. *c is the window's own, zeroed to start. */
struct xbox_title_clock;
int xbox_FramebufferWindowTitleText(wchar_t *tb, int n, struct xbox_title_clock *c);
#endif

/* The host window's title. The title sets it, before the window opens or at
 * any time after; unset, it is the certificate's name, or "Xbox Recomp"
 * before there is one. */
void xbox_HostWindowSetTitle(const char *title);

#if !defined(_WIN32) || defined(XBOXRECOMP_WIN_VULKAN)
/* POSIX: the process main thread runs the window's event loop and guest_main
 * runs on a thread of its own (fb_present_sdl.c). Returns guest_main's exit
 * code. */
int xbox_HostWindowMain(int (*guest_main)(void));

/* When the graphics device is lost the window says so in a box and ends the
 * run; with a restart to offer the box has "Restart" as well. The program's
 * own restart (recomp_restart.h) is offered when it is available; a host
 * with another way sets it here. */
void xbox_FramebufferWindowSetRestart(void (*fn)(void));

/* The window's next present read back into `path` (BMP), whatever it shows.
 * The window's thread only (the overlay's test driver, RECOMP_OVERLAY_TEST);
 * the Metal and Vulkan presenters. */
void xbox_HostWindowShot(const char *path);
/* The next n new frames read back as <prefix><flip>.bmp (a sequence for a
 * shimmer measure, BO-171); a frame the window drops is not shot. Same
 * thread and presenters. */
void xbox_HostWindowShotBurst(const char *prefix, unsigned n);

/* The bug report's system.txt gets this host's displays, controllers and
 * GPUs (host_probe.c, xbox_report.h). may_init 0: a title runs, only what
 * SDL already has is read; 1: nothing else runs in the process (a report
 * from the command line), SDL's video and controllers are started for the
 * listing and the graphics API is asked for its devices. SDL hosts. */
void xbox_HostReportProbe(int may_init);
#endif

#endif /* BURNOUT3_VIDEO_PLAYER_H */

# Logging: one gate, quiet release builds

`src/platform/xr_log.h` (BO-125). The runtime and a port print with plain
`fprintf(stderr, ...)` at about a thousand call sites; a five-minute session
printed some 30 000 lines (90 % of them the per-frame `[NV2A] DMA_PUT` line).
The header is force-included into every source the toolkit's CMakeLists
builds and into the port's target (`xboxrecomp` INTERFACE), so the existing
calls go through one gate unedited.

| Level | How | Printed |
|---|---|---|
| error | `XR_ERROR(...)` | always, stderr |
| notice | `XR_NOTICE(...)` | always, stderr: version, device, display, audio device, pads, settings file, save folder |
| diagnostic | every other `fprintf`, `vfprintf`, `fputs`, `fputc`, `fwrite` to stdout or stderr, and `printf`, `puts` | only while diagnostic logging is on |
| program output | `XR_OUT(...)` | always, stdout: `--install`, `--data-status`, `--version`, `--bug-report` answers |

The gate decides by the `FILE *` only: a print to any other stream (a WAV,
a trace CSV, the report's files) is written as before. A dropped call
evaluates none of its arguments and formats nothing (`drop ? 0 : real(...)`):
with diagnostics off the cost of a quiet line, the per-frame ones included,
is one load and a compare.

**When it is on.** First that says anything: `RECOMP_LOG_DIAG=0|1`; a
developer switch (`RECOMP_TRACE`, `RECOMP_DEBUG`, `RECOMP_STDIO_LOG`,
`RECOMP_FLIP_MARK`, `RECOMP_HEADLESS`, `--headless`); the setting
`advanced.diagnostic_log` (Advanced > Diagnostic logging, live); the build:
off in a release build (`BO3_RELEASE_BUILD`), on in every other. A program
that never calls `xr_log_init` (the tests, the tools) prints everything.
A crash, a hang report or a fault turns it on for the rest of the run.

**Where the log goes.** Unchanged: the tee (`xbox_crash.c`) copies stdout and
stderr to `<save dir>/logs/run.log` (first 64 KB) and `run.tail.log`
(384 KB, rotated once), at most about 830 KB a run, the last three runs kept
as `prev1..3`; Android also logcat and `<files>/takedawn.log`. In a quiet run
only errors and notices reach them, and the kernel's own `xbox_log` writes
nothing (no `xbox_kernel.log`; its ERROR level is the HLE's bookkeeping, such
as unresolved ordinal slots, nothing a player can act on). The bug report says whether logging was on and
how to get a full log.

**Testing a release build with today's log** (launchers on test machines):
`export RECOMP_LOG_DIAG=1`. The release smoke scripts set it themselves.

Tests: `tests/xr_log`.

## The marked sites

| Level | Where | Line |
|---|---|---|
| ERROR | src/video/fb_present_sdl.c:844 | `SDL video unavailable (%s); running headless` |
| ERROR | src/video/fb_present_sdl.c:910 | `Vulkan swapchain unavailable (%s);` |
| ERROR | src/video/fb_present_sdl.c:959 | `CAMetalLayer unavailable (%s);` |
| ERROR | src/video/fb_present_sdl.c:978 | `SDL window/renderer failed (%s); running headless` |
| ERROR | src/video/fb_present_sdl.c:1017 | `SDL_RegisterEvents failed; running headless` |
| ERROR | src/video/fb_present_sdl.c:1042 | `cannot start the guest thread; running headless` |
| ERROR | src/video/fb_present_metal.m:533 | `FXAA pass unavailable` |
| ERROR | src/video/fb_present_metal.m:538 | `CAS pass unavailable` |
| ERROR | src/video/fb_present_metal.m:860 | `SMAA unavailable` |
| ERROR | src/video/fb_present_metal.m:866 | `SMAA unavailable` |
| ERROR | src/video/fb_present_metal.m:879 | `SMAA unavailable` |
| ERROR | src/video/fb_present_vulkan.c:445 | `SDL_Vulkan_CreateSurface: %s` |
| ERROR | src/video/fb_present_vulkan.c:1010 | `GLSL compile failed: %s", what, err` |
| ERROR | src/video/fb_present_vulkan.c:1241 | `post passes unavailable (Vulkan objects failed)` |
| ERROR | src/soundtrack/xst_host.c:186 | `cannot write the soundtrack database` |
| ERROR | src/soundtrack/xst_host.c:219 | `cannot read the folder; custom music is off` |
| ERROR | src/soundtrack/xst_host.c:313 | `cannot read the folder; custom music is off` |
| ERROR | src/soundtrack/xst_host.c:421 | `cannot play song` |
| ERROR | src/platform/recomp_save_dir.c:423 | `failed part way; the old folder` |
| ERROR | src/platform/recomp_save_dir.c:502 | `[SAVE] backup to %s failed` |
| ERROR | src/apu/apu_core.c:1441 | `Failed to allocate MCPXAPUState` |
| ERROR | src/apu/apu_sdl2.c:100 | `[SDL2] audio init failed` |
| ERROR | src/apu/apu_sdl2.c:155 | `SDL_OpenAudioDevice failed` |
| ERROR | src/enhance/enhance_cfg.c:136 | `config error: %s (file ignored)` |
| ERROR | src/overlay/xbox_settings.c:522 | `cannot write %s` |
| ERROR | src/overlay/xbox_settings.c:558 | `cannot write %s` |
| ERROR | src/overlay/xbox_settings.c:566 | `cannot replace %s` |
| ERROR | src/kernel/xbox_report.c:1764 | `bug_report: failed` |
| ERROR | src/kernel/kernel_bridge.c:1004 | `not found in dispatch!` |
| ERROR | src/kernel/kernel_bridge.c:8889 | `not found in dispatch!` |
| ERROR | src/kernel/xbox_memory_layout.c:3122 | `CreateFileMapping failed (error` |
| ERROR | src/kernel/xbox_memory_layout.c:3229 | `failed to map base view` |
| ERROR | src/kernel/xbox_memory_layout.c:3786 | `WARNING: NV2A aperture` |
| ERROR | src/kernel/xbox_memory_layout.c:3898 | `WARNING: MCPX aperture` |
| ERROR | src/kernel/xbox_memory_layout.c:3915 | `WARNING: flash aperture` |
| ERROR | src/kernel/xbox_memory_layout.c:3960 | `WARNING: could not map Xbox kernel VA` |
| ERROR | src/kernel/xbox_memory_layout.c:4103 | `WARNING: tiled aperture` |
| ERROR | src/kernel/xbox_memory_layout.c:4116 | `WARNING: tiled aperture` |
| ERROR | src/kernel/xbox_memory_layout.c:4131 | `WARNING: tiled aperture` |
| ERROR | src/kernel/xbox_memory_layout.c:4809 | `xbox_HeapAlloc: out of memory` |
| ERROR | src/kernel/kernel_missing.c:262 | `[FILE] missing %s -> %s` |
| ERROR | src/nv2a/nv2a_mmio_hook.c:377 | `Failed to allocate %u MB for VRAM` |
| ERROR | src/d3d/nv2a_pb_metal.m:434 | `command buffer error: %s` |
| ERROR | src/d3d/nv2a_pb_metal.m:645 | `MSL compile failed: %s` |
| ERROR | src/d3d/nv2a_pb_metal.m:2230 | `MSL compile failed: %s` |
| ERROR | src/d3d/nv2a_pb_metal.m:655 | `MSL functions missing` |
| ERROR | src/d3d/nv2a_pb_metal.m:677 | `buffer creation failed` |
| ERROR | src/d3d/nv2a_pb_metal.m:681 | `pipeline cache allocation failed` |
| ERROR | src/d3d/nv2a_pb_metal.m:1117 | `partial-clear pipeline failed` |
| ERROR | src/d3d/nv2a_pb_metal.m:1526 | `LOGP "pipeline failed: %s` |
| ERROR | src/d3d/nv2a_pb_metal.m:2554 | `pcache: cannot append` |
| ERROR | src/d3d/nv2a_pb_metal.m:2808 | `pcache: cannot write %s` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:663 | `vkQueueSubmit failed` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:1501 | `GLSL compile failed: %s` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:1513 | `vkCreateShaderModule failed` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:1978 | `LOGP "pipeline failed: %s` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:4149 | `pcache: cannot append` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:4384 | `pcache: cannot write %s` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:4506 | `timeline semaphore failed` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:4523 | `command pool failed` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:4545 | `ring buffer %d failed` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:4651 | `base shaders failed` |
| ERROR | src/d3d/nv2a_pb_vulkan.c:4656 | `base textures failed` |
| ERROR | src/d3d/vk_dyn.c:147 | `could not be opened through` |
| ERROR | src/d3d/vk_dyn.c:183 | `no Vulkan loader: %s` |
| ERROR | src/d3d/vk_dyn.c:206 | `no Vulkan loader: %s` |
| ERROR | src/d3d/vk_dyn.c:250 | `no Vulkan loader: %s` |
| ERROR | src/d3d/nv2a_vk_glsl.c:167 | `SPIR-V cache: cannot append` |
| NOTICE | src/video/fb_present_sdl.c:501 | `[PRESENT] display %d mode` |
| NOTICE | src/video/fb_present_sdl.c:507 | `[PRESENT] window: display %d` |
| NOTICE | src/video/fb_present_sdl.c:996 | `window up: Vulkan swapchain (zero-copy)` |
| NOTICE | src/video/fb_present_sdl.c:999 | `window up: CAMetalLayer (zero-copy)` |
| NOTICE | src/video/fb_present_sdl.c:1004 | `window up: SDL %s renderer` |
| NOTICE | src/input/xinput_device.c:238 | `opened; it gets a port at its first input` |
| NOTICE | src/apu/apu_core.c:717 | `[APU] speakers: %s%s -> the title mixes` |
| NOTICE | src/apu/apu_sdl2.c:200 | `[SDL2] audio initialized` |
| NOTICE | src/enhance/enhance.c:117 | `present.filter=%s present.fullscreen=%d` |
| NOTICE | src/enhance/enhance.c:121 | `display.widescreen=%d display.hdtv=%s` |
| NOTICE | src/overlay/xbox_settings.c:797 | `is set in the environment` |
| NOTICE | src/d3d/nv2a_vk_core.c:341 | `[vulkan] device up: %s` |
| NOTICE | src/d3d/nv2a_pb_metal.m:684 | `LOGP "device up: %s (unified memory` |
| NOTICE | src/overlay/xbox_settings.c:747 | `LOGP "%s", s_path` |
| NOTICE | src/overlay/xbox_settings.c:611 | `%s: no file yet` |
| ERROR | src/overlay/xbox_settings.c:619 | `(file ignored, the next change replaces it)` |
| ERROR | src/game/main.c:167 | `fprintf(stderr, "%s", headline);` |
| ERROR | src/game/main.c:256 | `Cannot open XBE: %s` |
| ERROR | src/game/main.c:1577 | `[BOOT] flat dispatch unavailable` |
| ERROR | src/game/main.c:1588 | `Xbox memory layout init failed` |
| ERROR | src/game/main.c:2271 | `"usage: %s [--headless]` |
| ERROR | src/game/main.c:2049 | `"bug report: %s", s_report_path` |
| ERROR | src/game/main.c:2617 | `this build has no bug report` |
| ERROR | src/game/main.c:2645 | `--record/--seconds are not implemented` |
| ERROR | src/game/main.c:2657 | `--record/--seconds are not implemented` |
| ERROR | src/game/main.c:1262 | `[REC] ffmpeg failed` |
| ERROR | src/game/shaderbuild.c:713 | `"usage: burnout3 --build-shaders [options]` |
| NOTICE | src/game/main.c:2356 | `"[BOOT] %s%s", bo3_version_long()` |
| NOTICE | src/game/main.c:2073 | `[SAVE] copied ./%s to %s` |
| NOTICE | src/game/main.c:2078 | `[SAVE] backup: %s (keeping` |
| NOTICE | src/game/main.c:2079 | `[SAVE] dir: %s` |
| NOTICE | game src/game/install.c | `[DATA] root`, `[DATA] opening the setup window` (3) |
| ERROR | game src/game/install.c | the data-missing and setup-window failures (4) |
| NOTICE | game src/game/main.c | `[LOG] diagnostic logging is off ...`, `[SAVE] <import report>` |
| NOTICE | src/overlay/xbox_settings.c | `[LOG] diagnostic logging on/off` (the switch) |

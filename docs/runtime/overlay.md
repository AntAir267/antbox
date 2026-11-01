# The in-game settings overlay

Dear ImGui over the window's Metal or Vulkan present (`src/overlay`,
`XBOXRECOMP_OVERLAY=ON`, which needs `XBOXRECOMP_ENHANCE`). The SDL-renderer
fallback window and Windows have none yet (Windows follows with the D3D11
path).

## Opening it

| Where | How |
|---|---|
| Controller | Back + Start together (on the Steam Deck: View held + Menu, which arrive as Back + Start) |
| Keyboard | F1 |
| macOS | Cmd + , |
| Android | the system Back (button or gesture): it does not end the game, it opens the overlay; inside, it goes back like B. Leave through the app switcher |

Close with B or Esc. `RECOMP_OVERLAY=0` turns it off. For scripted runs:
`open` starts it open, `toggle=<s>,<s>` opens and closes it at those seconds,
`file=<path>` toggles it whenever that file appears (and removes it); a
`RECOMP_WINDOW_SHOT` pending while it is open is taken of the overlay over
the last frame.

## Look and sizes

The overlay uses the whole window (it pauses the game anyway), over the
frame dimmed to 50%, with a dark plate (the header band's colour at 72%)
behind the rows and the hint, as the title's own pause menu keeps its frame
bright and puts its text on dark plates (BO-190; no blur). Every size is a fraction of the window, the game's own
proportions measured on its 640x464 menu frame: label 3.7% of the height,
row pitch 6.25%, hint 2.6%, footer text 2.8%, header 4.7%; labels start at
14% of the width, values are right-aligned in a column from 56% to 94%, with
the arrows outside it; no upper cap, a floor of 13 px for a tiny window. The
content is at most 1.8 H wide and centred. A touch device (Android;
`RECOMP_OVERLAY_TOUCH=1` elsewhere, `RECOMP_OVERLAY_DPI` to force the
density) keeps a row at least 7 mm high. A text that does not fit its column
shrinks to 72% and is then cut. The selected row shows the value between
arrows; a switch is an On / Off value, a slider a bar of blocks. The palette
is the game's menu colours (title red, selected, list items, disabled,
hint, arrows, rule, footer bar, attention yellow: `k_theme_game`). The font
is still the monospace one. The Mixer page and the folder browser use the
same sizes and colours in a body under the header, their layout as before
(the Mixer's rows a little denser).

The rows come in the way the title's menus bring theirs (BO-190, timed from
per-flip captures of Burnout 3 at 60 Hz): each row slides in from 12% of the
width to the left and fades up in 2 frames (33 ms), the next one 3 frames
(50 ms) later after a page change, 4 frames (67 ms) later when the overlay
opens; rows below the sixth in view come in with it. It is drawing only:
the rows' vertices are moved and faded after they are made, so input works
on the rows where they end up from the first frame and nothing waits for it.
The header, footer and dim fade in over 0.14 s as before.

## What it does to the title

* **Input.** While it is open `xbox_InputGetState` (src/input/input_core.c)
  returns neutral pads and keys, so the title sees nothing, and the title's
  rumble is silenced. After it closes a port stays neutral until its buttons
  have been let go, so the press that closed it is not the title's. This is
  at the one function the USB pad model, the XInput path and the keyboard
  all read, not in each backend.
* **Pause.** `xbox_HostPause` (src/kernel/kernel_pause.h). The executor (the
  nv2a-ack thread, standing in for the GPU) holds at the end of FLIP_STALL;
  the title runs into its own fence and pushbuffer-space waits within a
  frame or two and stops there, as it does when a console's GPU stalls. No
  guest thread is suspended from outside and no lock is held across the
  hold, so it cannot deadlock with what the title holds. The APU's frame
  thread idles (its existing `pause_requested` path, via a separate
  `host_paused` flag the title's own register writes do not clear), so
  sound stops. The game time is not frozen: Burnout 3 steps its simulation
  by elapsed time with a limit of four steps a frame (sub_001B58E0), so it
  catches up by at most that.
  The pause takes hold at the next flip, so a load or movie that does not
  flip finishes first; the overlay opens at once.

Because Back+Start opens the settings, the title sees a pad's Back a moment
late, as in SpeedBreaker (`xbox_InputSetComboGuard`, per pad, the pad only; the
keyboard is untouched). Back down: neither it nor Start reaches the title. Start
while Back is down: the overlay opens and the title never sees either. Back
let go alone: the title gets a Back press then (`XBOX_INPUT_BACK_TAP_MS`, at
least two reads). Back held `XBOX_INPUT_BACK_DELAY_MS` (250 ms) without Start:
passed on as a held Back, with Start still hidden while it is. Start first is
not delayed (a grace would put its wait on every pause); Back after it still
opens the overlay. The same combo closes the overlay. Described in
`src/input/README.md`, "Overlay hotkey".

## Post passes and anisotropy

Anisotropic filtering applies to a mipmapped sampler with a linear
minification filter only (never a point-filtered one, never a render-target
read). FXAA and CAS run in the window presenters (`src/video/post_shaders.h`,
one neutral-dialect source for MSL and GLSL) at the game's internal
resolution, after the frame is final and before the scale to the window; the
overlay is drawn after them.

SMAA 1x is the reference implementation (`third_party/smaa`, MIT) at its
HIGH preset: edge detection by luma, blending weights from the area and
search tables, neighbourhood blending: three passes in place of FXAA's one.
`src/video/smaa.c` gives both presenters the same source (GLSL as it is;
for Metal a few exact text substitutions, checked at run time). A target
exists only while its setting is on (made at the first frame that needs it,
freed when none does), so switching live costs nothing while it is off.
`RECOMP_TRACE=post_gpu` prints each pass's GPU time every 300 presents
(Metal: the pass's own command buffer; Vulkan: timestamp queries, which a
tile-based GPU such as MoltenVK's does not give meaningfully). The
`aa_cycle=<s>` form of `RECOMP_OVERLAY` steps the setting for such a test.

## LOD bias and dither (both off by default)

`graphics.lod_bias` adds a mip LOD bias to every mipmapped texture, on top of
the title's own (SET_TEXTURE_FILTER's bias field). It is added to the per-draw
`tex_bias` constant that both backends hand the pixel shader (Metal's
`bias()`, GLSL's `texture(..., bias)`), because Metal has no sampler-state LOD
bias; so it composes with the title's bias by plain addition and with
anisotropic filtering as any shader bias does (the sampler's anisotropy and
min/max LOD clamps act on the biased level). A stage that is not mipmapped
gets none. `off` (default) adds exactly 0.0: the picture is the one it was.
`auto` is -log2(render scale) / 2: -0.5 at 2x, -1.0 at 4x (about -0.29 at
1.5x), 0 at 1x; `-0.5`, `-1`, `-1.5` are explicit. The environment takes any
number: `RECOMP_GFX_LOD_BIAS=-0.75`. Metal and Vulkan share the code
(`nv2a_host_lod_bias`).

Not on a cut-out (BO-171): a draw with the alpha test on (trees, bushes,
fences, road markings; Burnout 3 tests alpha > 64) takes no negative host
bias, only the title's own. The render scale already picks levels
log2(scale) finer, and a still finer level gives a leaf's edge more texels
than pixels, so it shimmers. A positive value still applies. Such a draw's
texture also gets its levels 1.. made again from level 0 when it is first
bound (`src/kernel/nv2a_texmip.c`): the alpha of each level is box-filtered
and then scaled so that as many texels pass the test as on level 0 (Castano's
coverage-preserving mipmaps), colour averaged by alpha, no gamma. The title's
own levels are a plain box, which at a reference of 64 lets a chain-link
fence fill in solid and a railing of thin bars vanish a few levels down; the
remade levels keep both as they look up close. Level 0, opaque textures and
every texture no alpha-tested draw binds stay as the title made them; the
levels are BGRA8 as before, so no memory is added (0.06 ms for a 128x128,
0.2 ms for a 256x256 on an M-series CPU, once per texture).
`RECOMP_DEBUG=alpha_mips=0` gives the old picture (`=mips` or `=bias` one
half only), `alpha_mips_ab=<n>` swaps both every n flips for comparisons, and
`alpha_mips_log` prints each texture's coverage per level before and after.

Many of the title's world textures (the road among them) have one level only
although their filter asks for mips; no bias reaches those, which is why
`auto` changes little on the road at a distance.

## 2D art upscaling (`graphics.ui_scale`, off by default)

PPSSPP's texture scaling for the 2D art only: a texture the title draws its
HUD, menus and fonts with is scaled 2x or 4x on the CPU, once, when a GPU
backend uploads it (`src/kernel/nv2a_texscale.[ch]`; hooks in
`nv2a_pb_metal.m` and `nv2a_pb_vulkan.c` after `nv2a_tex_decode` and
`nv2a_texrep_apply`, so replaced button prompts are scaled too).

Which textures, by how they are used and what they are (no list of a title's
textures, no hashes):

- a draw with the depth test off and no depth write, into the frame's own
  surface (as wide as what the title presents; not a smaller effect target),
  binds it. From then on the texture is scaled for every draw that uses it;
- it is decoded by the ordinary 2D path from guest memory: not a render
  target, not a cube map, not a signed (bump) texture;
- a swizzled or DXT format (a linear one is a video frame or a copy of the
  screen), one mip level, at most 512x512 texels;
- opaque or not: photographs drawn in 2D (the World Tour's, the loading
  pictures, the static between screens) are scaled with the rest. Only the
  switch-on pass below, which cannot see how a texture is drawn, keeps to
  art cut out with alpha (one texel in sixteen not opaque).

The scaled texture gets the mip levels down to the title's size (2 levels at
2x, 3 at 4x) and a trilinear sampler over them, so an element drawn smaller
than the scaled texture is minified through them; texture coordinates,
`tex_scale` and everything else computed from a size keep the guest's size.
Nothing in the shaders reads a texture's size.

The scaler is Super-xBR (Hyllian, MIT; a C version of the three shader
passes, notice in `NOTICE`), on premultiplied colour with alpha as a fourth
channel and part of the edge measure, in the title's own (gamma) colour
values, as its blending and the GPU's bilinear filter work. It reads past a
texture's edge by repeating the edge texel (never the opposite edge). Inside
a sheet nothing knows where one cell ends: a result texel is made of the four
source texels around it, steered and slightly sharpened by those up to three
texels away, and held between the four's lowest and highest value (the
anti-ringing clamp), so a neighbouring cell's colour cannot appear where the
four nearest do not have it. That is the reach bilinear filtering of the
title's own texture has (one texel); cells packed with no gap meet as they do
unscaled. Fully transparent result texels beside visible ones take the
visible colour. 4x is two 2x steps.

`RECOMP_DEBUG=ui_scale_algo=bicubic` scales with a Catmull-Rom bicubic
instead, for comparison (its own cache files). The scalers use
single-precision arithmetic without fused multiply-add and no library
function, so one source gives the same bytes on every machine
(`tests/nv2a_texscale` holds a checksum per scaler and factor).

`RECOMP_DEBUG=ui_scale_log` prints one line per
texture and kind of draw that binds it (with `texrep_dump`'s hash), and the
time and memory of each scale.

Nothing is scaled on the render thread (`nv2a_texscale_store.c`). At upload
a picked texture goes to the GPU as the title has it, and its decoded texels
(hashed: `nv2a_texscale_key`) are queued for one worker thread. The worker
loads the scaled level 0 from the disk cache or scales it (rows on up to four
threads) and writes it; the backend asks again once a flip at the texture's
bind and swaps the scaled texture in, one texture a flip (a new texture
object, so draws already encoded keep the old one).

The disk cache is `<shader cache directory>/ui_scale/`
(`render.shader_cache`; per user, made from the user's own disc): a file a
texture, `<key>_<w>x<h>_x<factor>_<scaler><version>.tsc`, a 48-byte header
(what it is, a checksum of the texels) and the raw scaled level 0. Written
as `.tmp` and renamed; a file whose header, length or checksum is wrong is
deleted and the texture scaled again; `NV2A_TEXSCALE_VERSION` in the name
retires a changed scaler's files. Past 256 MB
(`RECOMP_DEBUG=ui_scale_cache_mb=<n>`, 0: no disk cache) the least recently
used files are deleted down to three quarters. No disk cache directory: the
worker still scales, only nothing is kept.

Filling it ahead of use: any run with the setting on writes what it draws,
so a title's shader pre-build run does (it must pass the setting to its
scenes and call `nv2a_texscale_flush` before it exits). When the setting is
switched on in the overlay, the backend goes through its texture cache
while the title is held for the pause (`xbox_HostPauseIdleFn`) and queues
every loaded texture of at most 256x256 that passes the texture half of the
rule, for the disk only (it cannot tell which are drawn in 2D, and the larger
one-level textures with alpha are mostly the world's); the row's note shows
`nv2a_texscale_progress`.

Off (the default) asks for factor 0: no draw is classified, nothing is
hashed or queued, no thread is started, and every path is the one it was. A
change of the setting reaches a texture at its next bind (the cache entry
remembers the factor it was uploaded at).

## World texture upscaling (`graphics.texture_upscale`, off by default)

The 3D world's textures upscaled on the GPU while playing, the way PPSSPP
does it (BO-245, `src/kernel/nv2a_texup.h`, `src/d3d/nv2a_texup_metal.m`):
FSRCNNX x2 8-0-4-1 or NNEDI3, 2x or 4x (`graphics.texture_upscale_scale`;
4x is two 2x steps, for textures up to 256 a side). A Steam Deck
(`SteamDeck=1`, or the Deck's DMI board for a start outside Steam:
`recomp_host_is_steam_deck`) is offered 2x only: the scale row is hidden there and a 4x
from settings.toml or the environment runs as 2x (cold 4x stalled the
executor for 90-240 ms on the Deck). Metal, and Vulkan; the rows show only
where a backend can do it.
NNEDI3 is bjin's nns16-win8x4 network (mpv-prescalers) in one compute pass.
Its result keeps the title's texels at the even positions, a quarter of a
texel off the grid the texture is drawn on; the recombine pass moves it back
by half an output texel (as mpv's OFFSET does), so the upscaled texture
lines up with the original at the same coordinates.

Which textures: a swizzled or DXT colour format, not signed, not a cube map,
a render target, a video frame or a replaced texture, 16..1024 a side (512
on handhelds), bound by a 3D draw (a vertex program, the depth test on) and
never by a 2D one, unchanged for two flips. The title's road surfaces stay
as they are unless `graphics.texup_roads` is on (a built-in list of texrep
hashes the title sets with `nv2a_texup_set_skip_list`, plus
`RECOMP_DEBUG=texup_skip=<file>`).

How: the original is drawn at first; the texture's result is looked up in
the disk cache on a worker thread; on a miss one compute job at a time runs
the model in chunks (rows, or tiles of one group's rows) in a command buffer
of its own committed after the frame. A governor decides each flip how many
predicted GPU milliseconds may run: at most 0.5 ms while the frame draws 3D,
4 ms otherwise, and only while the recent GPU p95 plus the chunk fits the
frame period less 2 ms; nothing for 2 s after a burst of uploads; two late
frames while computing pause it for 2 s, doubling up to 32. It corrects its
per-device cost table from the measured GPU times. The swap is a pointer
change at the texture's next bind once its buffer has completed. Level 0 is
the model's output (4x: levels 0 and 1), the title's own levels follow, and
the sampler's LOD clamps move down by the levels added. Colour: the model
runs on the BT.709 luma; the bilinear colour is moved by the model's luma
minus its own. Alpha: the title's texel under each one, nearest, never
filtered (alpha kill, cut-outs and masks are the title's). A memory cap
(1024 MB, 192 MB on handhelds; `RECOMP_DEBUG=texup_cap_mb`) sends the least
recently used upscaled textures back to the title's.

Before the race (`graphics.texture_upscale_preload`, "Upscale while loading",
on by default; BO-272): a title says once a frame where it is
(`nv2a_texup_set_loading`: play, its countdown before the GO, its loading
screen; Burnout 3 reads its game state and player 1's event phase). A
track's textures are first drawn in the countdown, so outside play the
governor drops the upload-burst rule and the doubling pause (a late frame in
the countdown skips one frame's compute), allows the frame's GPU room up to
4 ms (countdown) or 6 ms (loading screen; 2 ms until three buffers are
measured), up to 4 or 16 new jobs a flip within 1.5 or 6 ms of render
thread, and a 32 MB swap slice; a finished job or a disk result is swapped
in at the flip, not only at its texture's next bind; at high refresh the
period is the host grid's. Silver Lake, first visit, Mac M5, 90 s: in-race
jobs 217 -> 99 and chunk GPU 219 -> 107 ms, no added load time; what stays
are the textures first drawn after the GO. Off: every frame is play, the
jobs spread through the race as before.

Disk cache: `<shader cache directory>/texup/`, one `.tup` file a result,
named by the content hash, size, format, algorithm, scale, edge rule,
shader version and policy, with a header and a checksum (a damaged file is
deleted and the texture made again); 2048 MB, least recently used first
(`RECOMP_DEBUG=texup_cache_mb`, 0: none).

Shaders: LGPL-3.0-or-later files loaded at runtime
(`third_party/upscale_shaders/NOTICE.md`: provenance, changes, how to
replace them; `tools/shaders/upscale_wrap.py` regenerates them).

Debug: `RECOMP_DEBUG=texup_log` (jobs, swaps, every 300 flips the device
memory and counters), `texup_ctl=<file>` (a test's live switch: "algorithm
[scale [roads]]"), `texup_cap_ms=<racing>[,<idle>]`, `texup_cost=<x>`,
`texup_fp32=1`, `texup_max=<n>`, `texup_shaders=<dir>`.
`RECOMP_TRACE=frametime` has the columns `gpu_ms`, `tu_ms`, `tu_pred`.

Off: no draw is classified for it, nothing is hashed, queued or timed, no
thread or pipeline is made; a texture that was upscaled goes back to the
title's at its next bind.

`graphics.dither` adds interleaved gradient noise (Jimenez 2014) of just
under +-0.5 LSB to the Metal presenter's scale to the window, the last step
before the 8-bit drawable. A texel that is already an exact 8-bit value
(1:1 text, flat colour) rounds back to itself, so only filtered, in-between
values move by one level at most. The source frame is itself 8-bit, so the
gain is at the edges of its bands when the frame is scaled to the window.
Off (default) skips the branch. The Vulkan presenter ends in `vkCmdBlitImage`
and has no such pass yet. Not in the menu; settings.toml or RECOMP_GFX_DITHER only.

## Settings

Each is one RECOMP_* knob. Precedence, high to low: the environment, then
`settings.toml` in the save folder (or the file `RECOMP_SETTINGS` names,
`none` for no file), then the title's defaults, then the toolkit's. A setting
the environment sets is shown locked.

`xbox_SettingsInit` (before the host's `env_default` calls) reads the file
and sets the variables it names, without replacing any already set;
`xbox_SettingsGameDefault` tells it each default; `xbox_SettingsFinish`
(after the last) takes the values the environment ends up with. So every
reader of the environment, including the enhancements layer (which then ranks
the file over `enhance.toml`), sees the file without knowing it exists.

The file is TOML, not INI: the toolkit already reads it (`recomp_cfg.h`,
`enhance.toml`), values are typed, and sections map to the overlay's pages.
Only settings the user changed are written, so the others follow the
title's defaults when those change. Keys it does not know are kept.

| Key | Variable | Live |
|---|---|---|
| `display.fullscreen` | `RECOMP_PRESENT_FULLSCREEN` | yes |
| `display.aspect` (`4:3`, `16:9`) | `RECOMP_DISPLAY_WIDESCREEN` | restart: the title reads the dashboard's video flags at boot |
| `display.filter` | `RECOMP_PRESENT_FILTER` | yes |
| `display.overscan` (`off`, `crop`, `fill`) | `RECOMP_PRESENT_OVERSCAN` | yes |
| `display.fit` (`letterbox`, `fill16x10`) | `RECOMP_PRESENT_FIT` | yes |
| `graphics.render_scale` | `RECOMP_RENDER_SCALE` | restart |
| `graphics.aniso` (`off`, `2x` .. `16x`) | `RECOMP_GFX_ANISO` | yes (the sampler caches key on the level) |
| `graphics.aa` (`off`, `fxaa`, `smaa`) | `RECOMP_GFX_AA` | yes |
| `graphics.sharpen` (0..1) | `RECOMP_GFX_SHARPEN` | yes |
| `graphics.lod_bias` (`off`, `auto`, `-0.5`, `-1`, `-1.5`) | `RECOMP_GFX_LOD_BIAS` (`0`, `auto` or a number) | yes (a per-draw constant) |
| `graphics.dither` (bool) | `RECOMP_GFX_DITHER` | yes; Metal only. Not in the menu; settings.toml or RECOMP_GFX_DITHER only |
| `graphics.ui_scale` (`off`, `2x`, `4x`) | `RECOMP_GFX_UI_SCALE` | yes (the texture caches upload a 2D texture again at its next bind) |
| `graphics.texture_upscale` (`off`, `fsrcnnx`, `nnedi3`) | `RECOMP_GFX_TEXTURE_UPSCALE` | yes (a texture goes to its upscaled or its own form at its next bind); Metal on Apple silicon |
| `graphics.texture_upscale_scale` (`2x`, `4x`) | `RECOMP_GFX_TEXTURE_UPSCALE_SCALE` | yes |
| `graphics.texup_roads` (bool) | `RECOMP_GFX_TEXUP_ROADS` | yes |
| `graphics.texture_upscale_preload` (bool, on) | `RECOMP_GFX_TEXTURE_UPSCALE_PRELOAD` | yes |
| `audio.volume`, `audio.mute`, `audio.mute_background` | `RECOMP_AUDIO_VOLUME`, `_MUTE`, `_MUTE_BG` | yes |
| `audio.speakers` (`auto`, `stereo`, `5.1`) | `RECOMP_AUDIO_SPEAKERS` | restart (auto: 5.1 when the default output has 6+ channels) |
| `audio.fx_level` (0..1, default 0.25) | `RECOMP_AUDIO_FX_LEVEL` | yes; 5.1 only: the title's effect-send bins, where they play dry. Not used (and not shown) for a title whose GP effects image is modelled (`src/apu/apu_gp.h`) |
| `audio.lfe_lowpass` (`off`, `80`, `100`, `120`; default `120`) | `RECOMP_AUDIO_LFE_LOWPASS` | not on the page (the settings file or the variable only; a change in a running game fades over 21 ms). For a title whose GP effects image is modelled: the low-pass on the LFE (subwoofer) feed, 24 dB an octave. 120 Hz is what the console's Dolby Digital encoder did after the effects stage and is the stock behaviour; off is that stage's own full-range feed. For tuning by ear use the mixer's own `lfe_lowpass_hz`, which comes after this one: with both on the feed passes both |
| `audio.menu_sounds` (default on) | `RECOMP_AUDIO_MENU_SOUNDS` | live; the title's own menu sounds in the overlay (above, "The title's own menu sounds"). Audio page, only for a port that named them |
| `audio.mixer` (default off) | `RECOMP_AUDIO_MIXER` | yes; the host mixer (`src/apu/apu_bus.h`): buses over the title's voices, on the Audio page's Mixer page. Its mix is not in settings.toml but in `mixer.toml` beside it (`RECOMP_AUDIO_MIXER_FILE`), read again when it changes. Off, and on with the neutral preset, the output is the same as before |
| `audio.custom_music_dir` (a folder, default empty) | `RECOMP_AUDIO_MUSIC_DIR` | a text setting (`xbox_SettingsText`). The Audio page's first row, "Custom music folder", with a folder browser (its status line and notes right under it, the page's settings after them), when the port asked for it (`xbox_OverlaySetMusicFolder`); live if the port can (`soundtrack.md`) |
| `display.frame_pacing` (`auto`, `low_latency`, `smooth`; default `auto`) | `RECOMP_FRAME_PACING` | live (read twice a second); Display page, "Frame pacing", on Linux with the Vulkan presenter. `smooth` keeps one finished frame queued: each frame's present-to-display time (present_wait) gives the queue depth, and three frames in a row at the title's 60 with none queued present the frame once more, three with two queued skip one present (each logged as `[PACING] smooth:`, counted in the `[PRESENT]` exit line). The title's 30 fps stretches and redraws are not measured. +1 refresh of latency against an empty queue; a present up to ~29 ms late still makes its refresh. `low_latency` is FIFO without it (each frame at the first refresh it can make, the depth wherever the last hiccup left it). `auto` is `smooth` under gamescope (Steam Deck Game Mode, Bazzite), `low_latency` elsewhere. Needs FIFO and present_wait, and off gamescope three swapchain images; otherwise it is `low_latency`. Metal (`display.present_timing`), D3D11 and Android: no effect yet (BO-110) |
| `display.present_mode` (`auto`, `fifo`, `mailbox`) | `RECOMP_PRESENT_MODE` | a text setting, no row in the overlay; after a restart. Vulkan with vsync: `auto` and `fifo` are FIFO, `mailbox` is MAILBOX where the surface offers it. Never picked by itself: under gamescope MAILBOX turned every late present into a repeated frame and replaced early ones (BO-110). The queue under FIFO is `display.frame_pacing` |
| `audio.custom_music_at_start` (bool, default on) | `RECOMP_AUDIO_CUSTOM_AT_START` | with a music folder set, the port starts the title on the custom soundtrack the player last chose (BO-230). Audio page, right under "Custom music folder", greyed out (with a hint) while no folder is set; only for a title that registered the folder row. Read by the port at the title's start |
| `audio.custom_music_soundtrack` (text, default absent) | `RECOMP_AUDIO_CUSTOM_SOUNDTRACK` | a text setting (`xbox_SettingsText`) the port writes: the name of the soundtrack the player last confirmed in the title's own screen. `""` is kept in the file (it means EA GAMES TRAX was chosen); no key means nothing chosen yet (`xbox_SettingsTextIsSet`). No row |
| `audio.custom_music_level` (`off`, `quieter`, `match`, `louder`; default `match`) | `RECOMP_AUDIO_CUSTOM_LEVEL` | live; the level of a title's custom soundtrack against its own music (`src/apu/apu_level.h`): a loudness chain on the custom-soundtrack voices only (BO-246: K-weighted 3 s short-term loudness, gated at -50 LUFS and 10 LU under the song's running level; the gain toward the target from the song's gated integrated level, -20 to +15 dB, 3 s; a 2:1 soft-knee compressor 8 dB over the target; `RECOMP_AUDIO_CUSTOM_AGC=old` runs the first leveller of BO-132 instead), with `quieter` and `louder` at -4 and +4 dB on the target and `off` no processing at all (bit for bit the title's sends). Audio page, under "Custom music folder", only for a title that registered the folder row. Needs the port to name its custom-soundtrack voices (`apu_level_set_classifier`); a title that does not is not touched. `RECOMP_AUDIO_LEVEL_LOG=1` logs the readings, the gain, the compressor and the limiter's count once a second. Separately, always on: a no-look-ahead peak limiter at -0.5 dBFS in front of the 16-bit clamp (`src/apu/apu_limit.h`), 50 ms release, which changes nothing while the mix stays under the ceiling (`RECOMP_AUDIO_SAFETY_LIMITER=0` switches it off, for comparison) |
| `game.speed_units` (`mph`, `kmh`) | `RECOMP_SPEED_UNITS` | live; Display page, only when the title enabled a guest patch for it (`recomp_patch_enable`, lifting: Guest patches) |
| `game.language` (`english`, `japanese`, `german`, `french`, `spanish`, `italian`, `korean`, `chinese`, `portuguese`, or the number 1..9; default `english`) | `RECOMP_GAME_LANGUAGE` | restart; the dashboard language the kernel reports (`XC_LANGUAGE`, `xbox_GetLanguage` in `src/kernel/kernel_xbox.c`), from which the title picks its text, speech and films. Display page, "Language", only when the title named more than one language its disc has (`xbox_SettingsSetLanguages`); the row steps over the others. Unset, English, as before |
| `display.ultrawide` (`off`, `21:9`, `32:9`, `window`) | `RECOMP_DISPLAY_ULTRAWIDE` | live, from the title's next 3D set-up; Display page, only when the title enabled a guest patch for it |
| `display.hud_fix` (bool) | `RECOMP_GFX_HUD_FIX` | live, shown whole from the title's next HUD set-up; Display page, only when the title enabled a guest patch for it |
| `display.score_board` (`original`, `smooth`) | `RECOMP_GFX_SCORE_BOARD` | live, from the next frame; a title hook on its HUD (Burnout 3: the SCORE board; the port's `src/game/scoreboard.c`): `original` touches nothing, `smooth` fades the board's plate with its text and fades the board in after a camera; Display page, only when the title enabled it (`recomp_patch_enable`) |
| `display.rival_marker` (`original`, `farther`) | `RECOMP_GFX_RIVAL_MARKER` | live, from the next frame; where the marker over a rival changes from its far look to its near one, by guest patches at `farther` (Burnout 3: the arrow and the race position, 35 units checked every 30 steps as shipped; farther: the position below 60, the arrow past 75, checked every step); Display page, row "Rival marker switch", only when the title enabled a guest patch for it |
| `display.crashnav_map` (`stretched`, `native`) | `RECOMP_DISPLAY_CRASHNAV_MAP` | live, from the next frame; with `display.hud_fix` on in 16:9, a title's map screen (Burnout 3: Crash Nav's Select Location) across the picture with its route and markers (stretched), or 4:3 with the rest of the menu (native); Display page, row "Crash Nav map", only when the title enabled a switch for it and `display.hud_fix` is on |
| `graphics.bloom`, `graphics.motion_blur` (0 to 1 in tenths, default 1, a slider in percent; a file's or variable's `false` / `true` reads as 0 / 1) | `RECOMP_GFX_BLOOM`, `RECOMP_GFX_MOTION_BLUR` | live, from the next frame; the title's own effects: a guest patch skips them at `0` and scales their weight in between (BO-212); Graphics page, only when the title enabled a guest patch for them |
| `graphics.crash_60fps` (bool, default off) | `RECOMP_GFX_CRASH_60FPS` | live, from the title's next frame; the title's own 30 Hz phases (Burnout 3: crash and takedown cameras) at 60, by a guest patch on the title's own rate choice at `1`; Graphics page, only when the title enabled a guest patch for it; for strong machines: a host that cannot hold 60 runs the game itself slower while it is on (the title steps once a frame), so the row says to leave it off on a Steam Deck and other handhelds |
| `graphics.softbody` (bool, default off) | `RECOMP_GFX_SOFTBODY` | read at start (a restart applies it); the title's own soft-body vehicle damage, for a title that has one (Burnout 3: Takedawn's `src/game/softbody.c`): vehicles crumple, tear and bend where they are hit, drawn only, handling unchanged; Graphics page, only when the title enabled it; costs CPU in a pile-up |
| `graphics.split_60fps` (bool, default off) | `RECOMP_GFX_SPLIT_60FPS` | from the title's next two-player stage; the title's 30 Hz split screen at 60, by a guest patch at `1`; Graphics page, only when the title enabled a guest patch for it; for strong machines: a host that cannot hold 60 runs the game itself slower while it is on (the title steps once a frame), so the row says to leave it off on a Steam Deck and other handhelds |
| `game.race_clock` (bool, default off) | `RECOMP_GAME_RACE_CLOCK` | from the title's next race; on a host that cannot hold 60, a race keeps real time from its start: the title's own catch-up stepping, which it otherwise starts only at the end of the first crash camera, by a guest patch at `1`; no change at a steady 60; Graphics page, row "Potato mode", only when the title enabled a guest patch for it |
| `audio.split_music` (bool, default off) | `RECOMP_AUDIO_SPLIT_MUSIC` | live; the title's music in split screen, where it plays none, by a guest patch at `1`; Audio page, only when the title enabled a guest patch for it |
| `graphics.vehicle_detail` (`original`, `far`, `farthest`) | `RECOMP_GFX_VEHICLE_DETAIL` | live, from the next frame; the distances at which the title swaps its vehicle models for simpler ones, scaled by a guest patch (2x, 4x); Graphics page, only when the title enabled a guest patch for it |
| `graphics.scenery_detail` (`original`, `far`) | `RECOMP_GFX_SCENERY_DETAIL` | live, from the next frame; the distance at which the title swaps a scenery object's detailed model for its simple one (Burnout 3: trees, signs and the other track props with two models), scaled by a guest patch at `far` (the factor is the title's: 4x in Burnout 3); Graphics page, row "Scenery detail", only when the title enabled a guest patch for it |
| `input.deadzone`, `input.trigger_deadzone` | `RECOMP_PAD_DEADZONE`, `RECOMP_PAD_TRIGGER_DEADZONE` | yes |
| `input.rumble` (strength, 0 to 3 in steps of 0.25; 1 the original, 0 off; motor = (speed / peak) ^ (1 / strength), see src/input/README.md) | `RECOMP_INPUT_RUMBLE_GAIN` | yes |
| `input.glyphs` (`auto`, `xbox`, `xbox_modern`, `playstation`, `playstation5`, `nintendo`, `steam`, `positional`, `keyboard`) | `RECOMP_INPUT_GLYPHS` | live: the title's button prompts drawn for the pad in use (`src/input/pad_glyphs.h`). `auto` follows the pad that last pressed a button (the keyboard counts, and shows key caps); `xbox` is the title's own picture, nothing replaced; `positional` is for any pad (no letters: a diamond of four dots, the one to press filled; `auto` picks it for a pad of unknown family, and the title's own picture while no pad has been used). Only a title that registers its prompt textures and brings pictures for them has anything to change (`src/kernel/nv2a_texrep.h`; Metal and Vulkan backends) |
| `power.mode` (`stock`, `performance`, `battery`, `auto`) | `RECOMP_PRESENT_PACING` + `RECOMP_PRESENT_SPIN_LEAD` (`auto`: `sleep` + `auto`) | yes |
| `advanced.perf_overlay` | `RECOMP_OVERLAY_PERF` | yes |
| `advanced.diagnostic_log` (bool; default on, off in a release build) | `RECOMP_LOG_DIAG` | yes; the print gate (`logging.md`). The row shows where the log is and what set it when the environment did |
| `advanced.perf_steps` (bool) | `RECOMP_OVERLAY_PERF_STEPS` | yes; the graph marks frames of more than one simulation step, when the port reports them (`xbox_PresentCadenceSteps`) |
| `display.present_timing` (`legacy`, `auto`; default `auto` on macOS, `legacy` elsewhere) | `RECOMP_PRESENT_TIMING` | yes, within half a second; Display page, Metal windows only. `legacy` presents a frame when it is ready; `auto` aims each frame at one refresh (`present-timing.md`) |

Power modes: *stock* is the busy-wait; *performance* sleeps and spins the
last 8 ms before each frame (steady frame time, about 10 W on the Deck);
*battery* sleeps throughout (about 8 W, small dips).
*auto* (BO-175) sleeps and picks the spin lead as it runs
(`src/kernel/power_auto.h`): 0, 2, 4, 8, 12 or 17 ms (all of the wait),
starting at 8. Every held flip is measured against the vblank the title asked
for, its own swap interval, so the title's 30 fps phases (crash cameras,
split screen) are on time, not misses. A 2-second window with near misses
(a frame late by less than a vblank) or late wake-ups (> 1 ms after the
vblank was raised, in 5 % of waits) steps it up at once, two levels when 2 %
of the frames missed; if the next window is still bad it climbs on, and if
even the top does not help it goes back and stays there for a while (the
GPU or the timer is the limit). In play (frames with half their interval or
less to spare, as in a race on a handheld) it never goes below 4 ms, which
keeps an APU's CPU clock up for the next bend, and steps down only after
30 s of clean windows; in menus after 10 s, down to sleep only. A frame one
to six vblanks late (a shader compile) skips that window's decision; a
stall (100 ms late, or 250 ms without a flip: a load) decides nothing for
3 s, undoes a climb that began less than 20 s before it, and takes the lead
back to the level last needed. It
changes only how the hold waits: the flip leaves at the same vblank. The
Advanced page shows the lead it holds and why it last moved;
`RECOMP_TRACE=pacing` prints its counts and each change.

The host's default is Balanced on every platform; the Burnout 3 port's macOS
default became Balanced too (BO-75, measured: as steady as Stock at about half
the CPU), with Maximum still offered there and a stored `power.mode = "stock"`
still honoured. *stock* (shown as Maximum) is offered on macOS only. On Windows, Android and
Linux / Steam Deck the Power page has Balanced (`performance`) and Battery
saver; a stored `stock` is read as `performance` at load (the file is not
rewritten for that; the next save writes the new value). The spin pacing
stays reachable for diagnosis with `RECOMP_PRESENT_PACING=spin`, which also
locks the row. The choice is one table, `k_stock_hidden` in
`src/overlay/xbox_settings.c` (BO-75; set an entry to 0 to offer Maximum
again); `xbox_PowerModeAtLoad(host, stored)` is the rule, testable per host
(`tests/overlay_settings`).

## Restart

Some settings are read once, when the game starts. The table in
`xbox_settings.c` says which with a flag, `XS_RESTART`, and that flag is the
only list: the row's RESTART tag and note, `xbox_SettingsNeedRestart`
and the Restart row all read it (the `overlay_settings` test names the
flagged ones, so adding a setting means deciding). Flagged today:
`display.aspect` (the dashboard bit the title reads at boot),
`graphics.render_scale` (render targets are made at start),
`audio.speakers` (the title picks its mix, and the device is opened, at
start), on Android `input.vibration`. Everything else is applied while
running or read each time it is used.

While a flagged setting differs from its value at launch, or a port asked
with `xbox_OverlayWantRestart(reason)`, or a save import waits, the foot of
every page shows one row, "Restart now to apply N changes" (N counts those
three), in attention yellow, over Quit game; its note has the reasons. A
changed setting's row carries a yellow RESTART tag beside its label; a
setting only read at start carries the same tag in grey. No page has a
Restart row of its own. Nothing restarts unless that row is picked. The row
closes the overlay (the settings are written), asks `recomp_restart`
for a restart and closes the window the ordinary way: the program's own
shutdown runs (motors off, audio closed, logs finished), and its last exit
handler starts it again with the arguments, directory and environment it
was first given (`src/platform/recomp_restart.h`): in place on Linux and
macOS (the same process id, so Steam, a launcher script, an AppImage's
runtime or a Mac bundle see one program throughout), as a new process on
Windows, through the activity (`restartGame()`) on Android. A port calls
`recomp_restart_init(argc, argv)` first thing in `main`; without that there
is no row, and the foot shows the yellow line "Restart the game to apply N
changes" instead (nothing to press).

### Restart now and Quit game (the foot of every page)

Every page ends with rows that stay in view below the page's own rows (the
rows scroll above them): "Restart now to apply N changes" (only while a
restart is pending) and "Quit game" (always). Both ask first: A (Enter, a
click) on the row shows "Press A again to restart / quit, B to cancel" in the
row and its note; the same row again does it. B or Esc, another row or
another page takes it back.

Quit game closes the overlay (settings written) and posts SDL_QUIT, the same
event as the window's close button, so the presenter's ordinary way out
runs: pads stopped, window and audio closed, `exit(0)`. Before that it calls
`recomp_quit_request` (`src/platform/recomp_restart.h`), whose host hook, set
by the SDL presenter on Android, calls `Activity.finishAndRemoveTask()`, so
the task does not stay behind. The Android part is not tested on a device.

## Save data: export and import

`xbox_OverlaySetSaveIO({save_dir, title_id, export_dir, note})` puts three
rows on the Advanced page, above the reset row (the functions are
`recomp_save_dir.h`'s; `tests/save_io` covers them on a made tree):

- **Export save to home folder**: copies the title's own save files,
  `UDATA/<title id>` and `TDATA/<title id>` of the save root, unchanged, to
  `<export_dir>/<YYYYMMDD-HHMMSS>/`, with a `save-export.txt` naming the
  title. Nothing else of the save root goes along. The full path is shown.
  The layout under the save root is the same on every host, so the dated
  folder can be carried to another machine's folder of the same name.
- **Import save from home folder (newest: ...)**: the newest export of this
  title in `export_dir`.
- **Import save from another folder...**: the folder browser (the music
  folder's, opened with other words: `xsb_open_with`), starting on "Exported
  saves", each folder marked when it is a save of this title.

An import never touches the save under the running title (the title holds
its save in memory and writes it back when it saves). The folder is checked
(it has `UDATA/<title id>` with files; its `save-export.txt`, if any, names
this title) and noted in the save root; the rows become "Cancel the import"
and the foot's "Restart now to apply 1 change". At the next start the
port calls `recomp_save_import_apply` before anything else looks at the save
root: the save in use is copied to
`<save root>/save-before-import/<date-time>/` (a folder of its own, which
the start-up backups' rotation never deletes), then replaced; if the copy
fails the old save is put back. The result is in the log (`[SAVE] Imported
the save from ...`) and under the rows for that run.

Messages, not silence: nothing to export yet, no export found, the target
cannot be written, not a save of this game, a save of another game.

Android: `export_dir` is in the app's own external files folder
(`/storage/emulated/0/Android/data/<package>/files/...`), the one place an
app writes without a permission; the row shows the full path. It is reached
over USB and by some file managers, not by all on Android 11 and later. A
place every file manager reaches (Download, Documents) needs the storage
access framework or a zip through MediaStore: not built.

## Save history: restore an earlier save

With the save guard on (`kernel_saveguard.h`, below) and a port that takes
a copy from its hook (`recomp_save_history_take`), the save root keeps
`save-history/<YYYYMMDD-HHMMSS>/`: the save as it was right before the game
wrote over it, each folder an export. Kept: the 20 newest, the newest of
each of the last 7 days and of each of the last 4 weeks; only dated, real
folders are removed (never a link, never a folder of another name).

**Restore an earlier save (N kept)...** (on the Advanced page with the
other save rows, when there is at least one) opens the folder browser in
`save-history`, newest first, the cursor on the newest. The folder chosen
is staged exactly like an import: "Restart now (the save is imported at the
start)", the save in use copied to `save-before-import/` first.

## Cheats (a tester's tool)

`xbox_OverlaySetCheats` registers up to four cheats, but their rows on the
Advanced page show only with `RECOMP_DEBUG=cheats`; players do not see them.
With the key set, a cheat on asks once more, and from the first one on the
title's saves go to a copy of the save (`<save root>/cheat-session`). When
the last goes off, the page asks whether to keep that progress (the save
goes into the save history first, then the copy replaces it) or throw it
away (the save stays as it was and saving waits for a restart). A copy left
behind by a quit is removed at the next start, with or without the key. A
port can also switch a cheat on without the row (the Burnout 3 port reads
`BO3_UNLOCK_ALL=1`; give such a run a scratch save).

## Saves are replaced whole (kernel_saveguard.h)

A port turns it on with `xbox_SaveGuardEnable("<title id>")` after
`xbox_path_init`. Then a file under `UDATA/<id>` or `TDATA/<id>` opened for
writing is a new file beside it (`<name>.~new`, a copy of the old one to
start with); at its last close a thread of the guard's own flushes it
(`F_FULLFSYNC` on macOS, `fsync`, `FlushFileBuffers`) and renames it over
the old name (`rename`, `MoveFileExW` with replace and write-through). The
title's close returns at once; a later open of the same file waits until
the rename is done. Same bytes as before: the new file is dropped and
nothing is renamed. Listings never show a `.~new`; one left by a process
that ended mid-save is removed at the next enable (the old file beside it
is the save). Every way out (`exit`, and the exit hooks of the windows that
end with `ExitProcess`) waits up to two seconds for a save on its way.
`tests/kernel_saveguard` ends a process at each step of a save and checks
the file is the old save or the new one, whole.

One running copy per save root: `recomp_instance_lock(save_root, wait_ms)`
(`recomp_instance.h`) holds `instance.lock` there for the life of the
process; a second copy is told and does not start. A restart gets it back.

## Text and fonts

Every string the overlay draws is UTF-8: its own labels and hints, a port's
rows, folder names in the music folder browser. Three faces, each a chain
Dear ImGui walks for a character (`overlay_fonts`, `gamefont_attach` in
`overlay_ui.cpp`); a glyph is rasterised when it is first drawn, at the size
it is drawn at, so there is no baked atlas and no list of ranges to keep:

| face | chain | used for |
|---|---|---|
| ui | Roboto Condensed Medium, then DejaVu Sans Mono | hints, the Mixer page, the folder browser, the graph |
| body | the title's text font (if registered), then ui's chain | the page name, tabs, row labels (capitals, leant), values, the footer keys |
| title | the title's large font, then its text font, then ui's chain | the one big word, SETTINGS |

| font | covers | licence |
|---|---|---|
| Roboto Condensed Medium, a subset (`overlay_font_ui.h`, 51 KB, 693 characters) | Basic Latin, Latin-1, Latin Extended-A and -B, Greek, Cyrillic, dashes and quotes, the euro and trade mark signs | SIL OFL 1.1 (`third_party/roboto_condensed/OFL.txt`, which a binary release carries) |
| DejaVu Sans Mono 2.37, a subset (`overlay_font_ext.h`, 41 KB, 630 characters) | the same scripts again, U+25A1 | Bitstream Vera licence (MIT kind; `third_party/dejavu/LICENSE.txt`, which a binary release carries) |

Roboto is made by `third_party/roboto_condensed/make_subset.py` from
google/fonts' variable `RobotoCondensed[wght].ttf` (checked by SHA-256),
pinned to wght 500.

### The title's own sprites and prompts

The same way, a port can name small DXT images inside a file of the
player's install (`xbox_OverlaySetGameArt`, `xbox_settings.h`: offsets and
sizes, the file's size and SHA-256; Burnout 3 names nine sprites of
`Data/Global.txd`): the up and left triangles and the tick of `Arrow`, the
`dpad` triangles, a rounded box's corner (`box_curve`, `16_curve` and their
`b` shapes), the `grid` tile, `big_curve` and the `Buttons` sheet. A thread
(`overlay_gameart.cpp`) reads and checks the file, decodes the sprites,
makes them 4x larger with Super-xBR and packs them into two RGBA textures
(one for the sprites, one for the prompt sheet of each of the ten glyph
sets) that Dear ImGui's backends upload like the font atlas; nothing is
written anywhere. They draw the cursor before the selected row, the arrows
beside a value and the page name, the value boxes (corners from the
sprites, sides plain, #101820 at 85% with a #385870 outline), the tick bar of
a percentage or scale row, the grid and the arc behind the rows, and the
footer: B Back, Y Default, the d-pad Change, A Select, as the buttons of the
pad in use (`xbox_OverlaySetPadSheets`: the port's per-set sheets, the
same images and rectangles it hands `nv2a_texrep`, over the title's own
`Buttons` sheet; `input.glyphs` and the active pad decide the set), or key
caps (Esc, Del, Arrows, Enter) when the last input was the keyboard; the
shoulder prompts sit beside the page names. The Mixer page and the folder
browser keep their text footer. No file or another file: the overlay draws
its own shapes and text as before (a log line, nothing on screen).
`RECOMP_OVERLAY_GAME_ART=0` forces that look.

More pictures come with `xbox_OverlaySetGameArtMore` (BO-190), each named
with its own file, size and SHA-256, and also in the 8-bit palette format
(Xbox swizzled, the palette after the pixels). Burnout 3 names `Volumebar`,
`Tick` and `circle` of `Data/Global.txd` and `Padlock` of
`Data/Frontend.txd`. A slider row draws the title's volume bar (full cells
white on the selected row, pale blue on the others, empty cells as blue
outlines) instead of the tick bar; a switch has the tick in its blue disc
(on) or a ring (off) at the left of its value box; a setting the
environment fixes has the padlock where its left arrow would be. Each file
is read once and checked on its own: one that fails costs only its own
pictures, and those rows keep the look above.

### The title's own menu sounds

A port can name the title's menu sounds the same way
(`xbox_OverlaySetMenuSounds`, `xbox_settings.h`): for a cursor move, a value
change, a confirm, a back, the menu opening and closing and a page turned,
a stretch of raw mono 16-bit PCM in a file of the player's install (offset,
length, rate, the level to play it at; the file's size and SHA-256), and the
host mixer bus whose gain applies. A thread (`overlay_sounds.cpp`) reads and
checks each file once, cuts the waves out and makes them 48 kHz; nothing is
written anywhere. A sound is a one-shot on the APU's output after the mix
(`mcpx_apu_ui_cue`, `src/apu/apu_core.c`), mixed into front left and right
like the test tone, so it goes out through whichever device backend is open
(SDL2 or XAudio2) with the master volume and mute. While the title is
paused the frame thread runs for the sound alone, as for the speaker test,
on the branch that hands the device the extra sound without a VP or GP
frame, so the title's voices stay where the pause left them; it idles again
when the sound is out. A new sound replaces the one playing. With the host
mixer on, the named bus's gain, mute and solo apply. `audio.menu_sounds`
switches them off (the row shows only when a port named sounds);
`RECOMP_OVERLAY_MENU_SOUNDS=0` names none.

Burnout 3 names three waves (`sound/Fe.awd` Vertical and Back,
`sound/Generic.awd` Select), mapped from a `RECOMP_MIX_TRACE` of its menus:
Vertical for a cursor move, a value change, a page turned and the menu
opening (the title plays it as each new page comes in, the pause menu
included), Select for A, Back for B and for the menu closing (the title's
pause menu plays Back as it closes); at the levels the front end plays them
(-15.5, -16.8 and -22.8 dB). When the title's own TAKEDAWN SETTINGS row opens
the overlay, the title has just played its Select: the overlay plays no
opening sound over it, and the pause waits 0.7 s so that Select is not cut
off and finished when the overlay closes (the title's input is held at
once).

### The title's own fonts

A port can name two bitmap fonts inside its title's executable
(`xbox_OverlaySetGameFonts`, `xbox_settings.h`): where each DXT5 atlas and
its glyph table are, the file's size and SHA-256. Burnout 3 names GlobalFont
and HeadFont of the US retail `default.xbe`. At start a thread reads the
player's own file, checks it, decodes the two atlases, scales them 4x with
Super-xBR (`nv2a_texscale`) and keeps the coverage in memory
(`overlay_gamefont.cpp`; about 90 ms on an M-series Mac); between two frames
the overlay then adds them in front of its own fonts. A font loader of the
overlay's own samples the 4x picture at whatever size a glyph is drawn, so
the letters stay sharp at 4K. Nothing decoded is written anywhere, and
nothing of the title is in this repository. No file, another file or a
table that does not read as glyphs: the overlay stays on Roboto everywhere,
scaled to the game font's capital height so the page keeps its proportions,
with one line in the log and no message on screen.
`RECOMP_OVERLAY_GAME_FONT=0` keeps Roboto.

The game's italics are its upright letters leant at draw time; the overlay
does the same (a shear of 0.2 applied to the drawn vertices, so a fallback
letter in the same string leans with them): the title, the page name, the
tabs and the labels are leant, values, hints and the keys upright. The
game's text font has Latin-1 and little more (no ő ű, no Cyrillic or
Greek): those letters come from Roboto, scaled to the game font's capital
height and set on its baseline, so "Zenék – ősz" mixes the two in one line.
The large font has capitals only.

A character no font has is drawn as a square (U+25A1), not as "?".
That is the case for CJK, Arabic, Hebrew, Indic scripts and emoji: out of
scope (a CJK font is megabytes, and right-to-left and combining scripts need
shaping this renderer does not do). A name in such a script still works: the
folder is found, chosen and stored by its real name; only its drawing is
squares.

Names from a disk may be decomposed (macOS stores "ő" as "o" + U+030B): the
browser's labels are composed first (`xst_utf8_compose`, Latin-1 and Latin
Extended-A), since the renderer does not stack combining marks.

## Shader pre-build

A host that can build its shaders ahead of time gives the overlay three calls
(`xbox_shader_build.h`, `xbox_OverlaySetShaderBuild`): start, cancel and poll
(state, fraction, text). The Graphics page then has a status line and an
action, *Build shaders now* (*Stop building shaders* while one runs, *Build
shaders again* after), with the host's own sentence on what it costs as the
hint. The overlay never opens by itself for it: the offer belongs to the
host's installer, and a player who skipped it there finds the row here. It
never starts a build on its own either. Closing the overlay stops a running build (the title is
paused while the overlay is open; a build beside a running title would cost
it frames), and the next one goes on from where it stopped. When a build
ends the host calls `nv2a_pcache_rescan`, and the backend builds what the
key list gained on its pre-warm thread.

What a build is (the scenes, the child process, the key list) is the host's
and the backends': see `nv2a_pcache.h`, `xbox_autoplay.h`, and Burnout 3's
`docs/shader-prebuild.md` for the one host that has it.

## Adding a graphics API

A backend is one file: init the official `imgui_impl_*` renderer with the
API's objects, and per frame call its NewFrame, `overlay_begin_frame()`,
`overlay_end_frame()`, its RenderDrawData (see `overlay_internal.h`), from
inside the presenter's pass. `overlay_metal.mm` and `overlay_vulkan.cpp` are
about 80 lines each.

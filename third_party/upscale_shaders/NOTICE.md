# Texture upscaler shaders (graphics.texture_upscale, BO-245)

These files are not MIT. They are LGPL-3.0-or-later, kept here as separate
files that the runtime loads and compiles when the setting is on. They are
not compiled into any binary. The full licence texts are `COPYING.LESSER`
(LGPL-3.0) and `COPYING` (GPL-3.0, which the LGPL builds on).

## Upstream, unmodified (`upstream/`)

| File | Upstream | Version | SHA-256 | Licence |
|---|---|---|---|---|
| `FSRCNNX_x2_8-0-4-1.glsl` | https://github.com/igv/FSRCNN-TensorFlow/releases/download/1.1/FSRCNNX_x2_8-0-4-1.glsl | release 1.1 | e800dbc5c1c95185cc82216c597724533ff5f2880179f256eef600f03e8dc2ae | LGPL-3.0-or-later (file header), Copyright (C) 2017-2021 igv |
| `nnedi3-nns16-win8x4.hook` | https://raw.githubusercontent.com/bjin/mpv-prescalers/b3f0a59d68f33b7162051ea5970a5169558f0ea2/nnedi3-nns16-win8x4.hook | bjin/mpv-prescalers commit b3f0a59d (2024-01-11; the file last changed in b3b4cb43, 2019-03-09) | f50a3f6acc4957bedca9fb1f7d58823e9f4ff149245e29d8013f3223e0830ebd | LGPL-3.0-or-later (file header, no copyright line), by bjin; NNEDI3 by tritical |

## Generated from them (this folder)

`tools/shaders/upscale_wrap.py` (MIT, in the toolkit) writes these from
`upstream/`; they are derived works under the same LGPL-3.0-or-later:

- `luma.comp/.metal`, `recombine.comp/.metal`: written for this project
  (the luma and the colour/alpha recombination), shipped with the rest;
- `fsrcnnx_00..12.comp/.metal`: the FSRCNNX hooks, each wrapped as a compute
  pass; the hook bodies (weights and equations) are the upstream text;
- `nnedi3.comp/.metal`: a single-pass 2x compute shader written for this
  project around bjin's network: the hook's `nnedi3()` function (weights
  and equations) is the upstream text unchanged; the wrapper runs its
  double_y and double_x hooks in one work group (16x16 input texels, the
  double_y strip in shared memory, every read with the texture's edge rule).
  The generator checks that double_x is double_y's network with the window
  transposed, so one function serves both. tests/nv2a_texup/check_nnedi3.py
  evaluates the hook file's two passes in NumPy to check it;
- `VERSION`: a hash of the generated files (part of the disk cache's key).

`.comp` is GLSL 450 (Vulkan compiles it at runtime with glslang); `.metal`
is the same through glslangValidator and SPIRV-Cross (Metal compiles MSL at
runtime).

## Replacing them

The runtime takes the first folder with a `VERSION` file from:
`RECOMP_DEBUG=texup_shaders=<dir>`; the user's override folder (macOS
`~/Library/Application Support/<app>/shaders/upscale`); the app bundle's or
the executable's `shaders/upscale`; a source build's
`third_party/upscale_shaders`. To use a modified shader: edit `upstream/`,
run `tools/shaders/upscale_wrap.py <folder>`, and put the folder in one of
those places. The log names the folder it loaded. A changed file changes
`VERSION`, so cached results of the old shaders are not used.

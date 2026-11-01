# antbox

A shared toolkit for statically recompiling original Xbox games into native programs:
the game's x86 code becomes C, with no emulator and no interpreter. It's a fork of
xboxrecomp, and it's what these ports are built on:

- Wreckless: The Yakuza Missions
- Double-S.T.E.A.L 2: Second Clash
- MechAssault 2: Lone Wolf

**antbox contains no game.** There are no game files, executables or generated code from a
game here. To build a port, you need your own copy of the game.

## What's different from xboxrecomp

- **A native Vulkan renderer** (`src/nvr`). It's new and off by default. Choose it with
  `RECOMP_PB_BACKEND=native` or the `graphics.renderer` setting. It keeps render targets
  on the GPU where the existing Vulkan backend copies them back to guest memory. HDR,
  texture upscaling and registered decal depth offsets aren't ported yet.
- **Frame capture and replay** (`src/nvr/nvr_capture.c`, `tools/nvr_replay`). Record a run,
  then replay it through either renderer without the game and compare frames pixel by pixel.
- **Lifter and runtime fixes** found on these games, such as `fld` and `fstp` of 80-bit
  reals, which C library trigonometry depends on.

## Platforms

| Platform | Status here |
|---|---|
| Linux, Vulkan | What these ports run on: NVIDIA RTX 4070 and Radeon 890M (RADV) |
| Windows, macOS, Android, other backends | Code from xboxrecomp, which we don't build or test |

## Building

antbox is a library: the executable lives in your game's own project, which starts from
`templates/new-game`. The pipeline (XBE parsing, disassembly, lifting to C) needs Python
3.10+ and `capstone`. The build needs CMake, Clang or GCC, SDL2, the Vulkan headers and
glslang; without the last two, CMake quietly leaves the Vulkan backends out.

```bash
git clone https://github.com/AntAir267/antbox.git
```

A project builds with `-DXBOXRECOMP_DIR=<path to antbox>` once its game has been lifted to C,
so there's nothing to run until you've done that. [Getting Started](docs/GETTING_STARTED.md)
walks through a whole game. Set `RECOMP_PB_BACKEND=vulkan` to draw with Vulkan, since a
new project doesn't choose a backend by itself.

## Documentation

- [Getting Started](docs/GETTING_STARTED.md)
- [Pipeline](docs/pipeline), [runtime](docs/runtime), [formats](docs/formats) and
  [technical notes](docs/technical)
- [Input](src/input/README.md)
- [xboxrecomp's README](docs/UPSTREAM-README.md), with its architecture, pipeline and
  full changelog

## How changes get here

This repository's history starts at its first public commit. Work happens in a private
repository, and each change is copied here after it passes the checks of the games above.

## Credits and license

antbox is built on [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp), through
[apexJCL/xboxrecomp](https://github.com/apexJCL/xboxrecomp) and the `takedawn` branch of
[xenophobentx/xboxrecomp](https://github.com/xenophobentx/xboxrecomp), the toolkit of
[Takedawn](https://github.com/xenophobentx/takedawn). [CONTRIBUTORS.md](CONTRIBUTORS.md)
records who did what upstream.

The toolkit is [MIT licensed](LICENSE). Some parts carry other licenses, such as LGPL code
from xemu: see [NOTICE](NOTICE) and [LICENSES](LICENSES).

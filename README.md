# whiwhi

A shell that hosts the original GTA VI executable (Xbox Series 1.0.2.1) on a PC GPU. Original work by ExCorvus
([ExCorvus](https://github.com/ExCorvus)).

The original game files are the source; the runtime here is our own code. You need your own copy of the game — this
repository contains no game data, keys or executable, and nothing here decrypts or bypasses any protection.

## Status

The shell loads the unmodified game file, connects its imports and starts it. The game runs its own start-up:
it sets up its memory, opens its package files, creates a graphics device (Direct3D 12 on Vulkan) and completes
its graphics start-up on it. It then stops when it creates its first texture, at the console's texture layout
library, which the shell does not provide yet. **Nothing is drawn on screen yet**: texture layout, shader
translation, most of the command-list interface and audio are not built. The exact state is the "Current
state" block at the top of [FINDINGS.md](FINDINGS.md).

## Files

```
shell/                         the runtime (C++17, MSVC); shell/README.md describes every file
  image, imports, loader       read the game's executable, place it in memory, connect its imports
  tls_seat, thread_block.h     thread-local storage for the loaded game
  host_libraries, console      decide which library answers each import
  hosting                      address model for showing the game's memory to the GPU
  shader_container, shader_isa, shader_translate, spirv
                               shader container reader, console shader reader, translator to SPIR-V, SPIR-V writer
  xmem                         xmem.dll - the console's memory library, answered on a PC
  d3d12_xs                     d3d12_xs.dll - the console's graphics entry points, over a PC D3D12 device
  xcrdapi                      XCRDAPI.dll - the console's file-read library
  xgameruntime                 xgameruntime.dll - the game runtime library the game loads at start
  acp_hal, xapu, chad          AcpHal.dll, xapu.dll, chad.dll - console audio libraries (skeletons)
  xg_xs, xbox_bvh_builder      xg_xs.dll, XboxBvhBuilder.dll - texture layout and ray-tracing builder (skeletons)
  tools/                       list_imports, describe_image, connect_imports, boot_image, vkhost_probe
  tests/                       14 tests, made-up inputs only
  CMakeLists.txt, build.cmd    build definition and the build-and-test script
docs/
  BUILD-AND-RUN.md             how to build, test and run the shell yourself
  exe-map/MAP.md               what the executable does: memory model, page mapper, watchdog
  exe-map/CHANNELS.md          every way the game hands a memory address to the GPU
  exe-map/GRAPHICS-CALLS.md    what the executable asks of the console graphics libraries
  exe-map/COMMAND-LIST.md      how the executable uses the console command list
  exe-map/SHADER-PATH.md       how the executable's shader bytes reach the graphics device
  console-libs/                what the executable asks of the audio, file-read and game runtime libraries
  SHADER-TRANSLATOR.md         the shader translation plan
  EARLY-HISTORY.md             the earlier attempt and why the project restarted
  RENDERING-PIPELINE.md        not written yet
scripts/
  check_no_game_data.py        check that refuses game data in files (run before every commit)
FINDINGS.md                    current state and the dated log of measurements
AGENTS.md                      rules for agents working on the project
CONTRIBUTING.md                code and commit conventions
ATTRIBUTION.md / NOTICE.md     who contributed what; the credit required on reuse
LICENSE                        GPLv3-or-later
```

## Build

```
shell\build.cmd
```

Builds the shell, the console libraries and the tools, and runs the 14 tests. No game files and no GPU are needed.
To run the shell against your own copy of the game, see [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md).

## License

Copyright (C) 2026 ExCorvus. Licensed under the GNU General Public License, version 3 or later — see
[LICENSE](LICENSE) and [NOTICE.md](NOTICE.md). Grand Theft Auto and related names are trademarks of their owners;
this project is not affiliated with or endorsed by them.

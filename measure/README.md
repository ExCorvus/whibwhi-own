# measure/ — repeat the measurements yourself

Every result in [FINDINGS.md](../FINDINGS.md) that was read from the executable should be repeatable by anyone
who has their own copy of the game. The scripts here do that: each one reads the executable **read-only, in
memory**, checks the statements of one group of FINDINGS entries, and prints `same` or `DIFFERS` for each.
Nothing of the game is written anywhere, and the scripts themselves hold only addresses and the numbers found
there (rule 2 in [AGENTS.md](../AGENTS.md)).

These are measuring tools, not part of the weave's build. The game test (`scripts\game_test.ps1`,
docs/BUILD-AND-RUN.md) runs every one of them on your copy before it starts the game, and its verdict is FAIL when
one says `DIFFERS`. It finds each script's file from the usage line (`"<your game folder>\\<file>"`), so a new
script needs that line; a script that checks results without it makes the game test fail rather than skip it.

## What you need

- Python 3.10 or newer, and `pip install numpy capstone` (capstone is the disassembler; it is installed
  separately and not part of this repository).
- Your own copy of the game, version 1.0.2.1. You pass the path of its executable to each script.

## Running

```
python measure/audio.py "<your game folder>\game_scarlett_master.exe"
```

The last line says how many statements are the same as FINDINGS.md; the exit code is 0 when all are.

## The scripts

| script | repeats |
| --- | --- |
| `pe_image.py` | not a measurement: the shared reader (sections, functions from the executable's own `.pdata`, callers, rip-relative references, the number or offset in one instruction) |
| `audio.py` | FINDINGS 2026-11-27, "the program's audio setup" and its follow-ups: the 350 sound records, the setup/start/stop/data commands, the XMA2 packet walk, what the device writes back, the stream channel's float output turned into 16-bit samples |
| `topology.py` | FINDINGS 2026-11-27, the topology entries: the renderer's topology field, the places that set rectangle lists (0x11) and quad lists (0x13), the rectangle drawn from three vertices, the class the draw path makes for the pipeline key |
| `dxil_bitcode.py` | not a measurement: the shared reader of shader containers and of the LLVM bitcode inside them (record kinds, declared functions, metadata text) |
| `builder_shaders.py` | FINDINGS 2026-11-26/27 about the installation's `XboxBvhBuilder.dll`: its 52 shaders, their parts, the root signature bits 0x3000000, stages and thread groups, insertvalue, the 33 console shader compiler functions, the 32-lane switch. Takes the library's path, not the executable's. Not yet repeated here: the operation numbers of those functions |
| `imports.py` | FINDINGS 2026-11-26, "the installation's own libraries": the executable's 68 libraries and 587 functions, the seven of those libraries that ship in the installation, and what the shipped builder, Bink and FidelityFX libraries ask of the console libraries |
| `watchdog.py` | what FINDINGS 2026-11-26 "the program's progress reports" stands on: the program's hang watchdog as docs/exe-map/MAP.md section 4 describes it (who writes the progress stamp, the one-second pass, the 2 s and 300 s limits, the 30 passes, the stop, the run loop, the watch scopes). It found two corrections to the map (2026-11-27) |
| `stops.py` | the places in the executable where FINDINGS says a run stopped or reached a new call: the audio setup's breakpoint at image+0x43ad94e (the ways that lead to it, the unwind chain of boot b169), the pipeline store checked before device slot 86, the builder's construction at image+0x247944f, the program's own compute shaders through device slot 62 at image+0x247fe66 |
| `memory.py` | docs/exe-map/MAP.md sections 1.1-1.3, 1.6, 1.7 (verified parts): the memory start-up (the two views, D3D12ConfigureVirtualMemoryX, the nine fixed pools and their allocators), the streaming page pool, the page mapper and its six operation kinds, who else touches the page tables |
| `descriptors.py` | docs/exe-map/DESCRIPTORS.md section C (measured parts): the descriptor record size, the five places where the program copies 32-byte descriptors itself and the device byte +0x18 that sends them to device slot 24, the canned null descriptors, the check that the type-0 heap starts at the requested address. It found a correction to C.2 (2026-11-27) |
| `graphics_calls.py` | docs/exe-map/GRAPHICS-CALLS.md sections 1.2 and 4: the one call that asks for the device (after the program's own window), its 64-byte creation block and arguments, what the set-up asks of the device right after, the builder library's seven methods and their call sites, the one builder object. It found a correction to 1.2 (2026-11-27) |
| `builder_pipelines.cpp` | FINDINGS 2026-11-27, "the builder's 52 shaders made with their own root signatures" (game test 01:34): all 52 compute pipelines made on one PC device, each with its own root signature, in the builder's measured order; no error line of the PC library; with `--validate`, no message of the Vulkan validation layer. Needs a PC and its graphics card (see below) |

## The one that needs the graphics card

`builder_pipelines.cpp` is a small C++ program, because it has to talk to the PC graphics library. It hands the
builder's shaders to your graphics driver to be compiled into pipelines, and runs none of them. It needs:

- the vkd3d-proton fork built as in [docs/BUILD-AND-RUN.md](../docs/BUILD-AND-RUN.md), and its `d3d12.dll`
  (renamed `d3d12_vk.dll`) and `d3d12core.dll` beside the built program;
- for `--validate`, the Vulkan SDK (its validation layer).

```
cl /std:c++17 /EHsc /O2 measure\builder_pipelines.cpp
builder_pipelines.exe "<your game folder>\XboxBvhBuilder.dll" --validate
```

The same program run beside an unchanged vkd3d-proton (71cae09) makes 9 of the 52: the 9 shaders that use none of
the console shader compiler's own functions. That is the check that it can tell a difference.

More scripts are added as the earlier results are turned into repeatable steps.

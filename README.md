# whiwhi

Original work by [ExCorvus](https://github.com/ExCorvus). You need your own copy of the game; this
repository contains no game data, keys or executable, and nothing here decrypts or bypasses any protection.

## Why a shell, and what it does

The Xbox Series build of GTA VI (1.0.2.1) is one 120 MB program file in the ordinary 64-bit Windows format, made
for the same kind of processor a PC has. A PC can execute its code directly: nothing has to be emulated and
nothing recompiled. What stops it from starting on a PC is everything the program expects to find *around*
itself. This is what was measured, and what the shell does about each:

1. **System libraries.** The program imports 587 functions from libraries that exist only on the console:
   memory, graphics, texture layout, file reading, audio, game runtime. The shell provides its own libraries
   under those names; 586 of the 587 are connected.
2. **Memory.** At start-up the game reserves two 24 GiB ranges through the console's memory library, one for
   the processor and one that only the graphics chip reads, plus nine fixed pools. On the console both chips
   share that memory. A PC graphics card has its own. The shell keeps the game's memory as normal PC memory and
   shows it to the graphics card whenever a graphics call names an address inside it.
3. **Graphics calls.** Only three graphics functions are imported; everything else goes through the objects
   they return. Those objects are the console's variant of Direct3D 12: the methods sit in a different order,
   some exist only on the console, the game reads and writes fields inside the objects itself, and it names
   buffers and textures by address in its own memory. The shell hands the game objects in the console's layout,
   wrapped around a real PC Direct3D 12 device (on Vulkan), and translates each call.
4. **Shaders.** A shader arrives in a container that looks like a PC one, but the part that should hold portable
   shader code holds none. In its place is a packed block with the program already compiled into machine code
   for the console's graphics chip. No PC driver accepts that. The shell unpacks the block in memory, reads
   every instruction, and writes an equivalent PC shader. The block also says which register carries which
   resource; the game's own binding description gives only the order and kind, so the shell has to build the
   PC-side binding for each shader itself.
5. **Files.** The game reads its packages through the console's storage library, whose objects are laid out
   differently from the PC's. The shell answers with its own objects in the console layout and reads, read-only,
   from your game folder.
6. **Audio.** The game asks for the console's audio processor and has no path for "there is none". A run
   currently ends there.

So the shell is neither an emulator (no console hardware is recreated) nor a port (the game's code is not
changed or rebuilt). It is the console's surroundings, rewritten for a PC, with the unmodified game placed
inside.

The measurements behind each point are in [FINDINGS.md](FINDINGS.md) and [docs/exe-map/](docs/exe-map/).

## What the earlier attempt got wrong

Before this shell there was an attempt built on top of another project's runtime (Leonida, by NotRexed). None of
that code is here. The attempt failed, and the reasons are why this project is shaped the way it is. The mistakes
described are our own.

1. **It turned failures into successes.** When a memory request of the game could not be met, the runtime was
   patched so that the request came back as "done". Each patch moved the run further: a crash went away, the
   frames got brighter. The game still never reached its world, because it was now running on answers that were
   not true.
2. **It worked from inside the game instead of from outside.** The effort went into the game's own allocation
   and error paths. But the game's logic is not what is broken on a PC. What is missing is its surroundings (the
   six points above). Fixing the inside hides that.
3. **It gave the game's memory to the graphics card piece by piece.** Single regions were imported, copies were
   kept, single addresses were corrected one at a time. The game has no table for that: it expects one memory
   that both chips see. Every piece that was missed became an address the graphics card could not reach, and the
   crash at the start of the game world was exactly such an address.
4. **It measured progress by how far a run got.** A run that gets further looks like progress whether or not
   the answers behind it are right.

What this project does instead, as fixed rules ([AGENTS.md](AGENTS.md)): provide what the game expects and never
change its checks; never answer "done" for work that was not done; treat the game's memory as one whole; and take
the program file itself, not the length of a run, as the reference. The longer account is in
[docs/EARLY-HISTORY.md](docs/EARLY-HISTORY.md).

## Where it stands

Points 1, 2, 3 and 5 work far enough that the game starts, sets itself up, opens all its packages, creates its
window and presents frames. **The window is black**: that is the game's cleared surface, because point 4 is
unfinished. The shell reads 927 of the first 1,024 shaders of a run and can write a PC shader for 281 of 505
pixel and 129 of 227 vertex shaders, but none has been given to the graphics card yet, so every draw is held.
The exact state is the "Current state" block at the top of [FINDINGS.md](FINDINGS.md).

## Files

```
shell/                         the runtime (C++17, MSVC); shell/README.md describes every file
  image, imports, loader       read the game's executable, place it in memory, connect its imports
  tls_seat, thread_block.h     thread-local storage for the loaded game
  host_libraries, console      decide which library answers each import
  offline                      refuses the game's network imports: no socket, no name lookup, no HTTP request
  hosting                      address model for showing the game's memory to the GPU
  shader_container, shader_program, shader_walk, shader_names, shader_isa, shader_shape,
  shader_flow, shader_emit_flow, shader_meaning, shader_access, shader_dxbc, shader_pc,
  shader_translate, spirv
                               shader container reader, unpacking the program a container carries, program
                               walk (instruction lengths), public instruction names, instruction reader,
                               what a program expects and uses, its blocks, branches and loops, the same
                               flow as nested code a shader language can run, what the most used
                               instructions compute, what memory, image, interpolation and export
                               instructions touch, a writer for the PC's shader container, the pass that
                               writes a PC shader for a console program (not yet given to the device), an
                               older translator to SPIR-V (test shaders only), SPIR-V writer
  xmem                         xmem.dll - the console's memory library, answered on a PC
  d3d12_xs                     d3d12_xs.dll - the console's graphics entry points, over a PC D3D12 device
  xcrdapi, inflate             XCRDAPI.dll - the console's file-read library, with our own zlib decoder
  xgameruntime                 xgameruntime.dll - the game runtime library the game loads at start
  acp_hal, xapu, chad          AcpHal.dll, xapu.dll, chad.dll - console audio libraries (no audio device)
  xg_xs                        xg_xs.dll - the texture layout library, answering with an untiled layout
  xbox_bvh_builder             XboxBvhBuilder.dll - ray-tracing builder (skeleton)
  tools/                       list_imports, describe_image, connect_imports, boot_image, vkhost_probe
  tests/                       27 tests, made-up inputs only
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
  SHADER-OPCODES.md            public names of the shader instructions the game uses most
  SHADER-MEANINGS.md           what each of those instructions computes, with the public source for it
  SHADER-ACCESS.md             what memory, image, interpolation and export instructions touch, with sources
  SHADER-CONTAINER-WRITER.md   the PC shader container the translator writes: layout and token forms
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

Builds the shell, the console libraries and the tools, and runs the 27 tests. No game files and no GPU are needed.
To run the shell against your own copy of the game, see [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md).

## License

Copyright (C) 2026 ExCorvus. Licensed under the GNU General Public License, version 3 or later — see
[LICENSE](LICENSE) and [NOTICE.md](NOTICE.md). Grand Theft Auto and related names are trademarks of their owners;
this project is not affiliated with or endorsed by them.

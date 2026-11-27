# whibwhi

*The web that threads us together.*

Original work by [ExCorvus](https://github.com/ExCorvus). You need your own copy of the game; this
repository contains no game data, keys or executable, and nothing here decrypts or bypasses any protection.


## What the weave is

whibwhi started as a shell: something wrapped around an unmodified program, answering what the program expects
from the console it was made for. By its code it still is one. But measuring the program showed that it is not
wrapped around the executable. It is woven all through it.

**A weave, not a web.** The program itself is the web: everything in it hangs together. The weave is the active
process, what happens while the executable runs: call by call, the program and the weave work through each other.
The name refers to that process, to how the executable behaves when it runs, not to a structure that sits still.

**A consideration for other games.** The weave is measured on one program so far. Every game on the Xbox is its
own web: it hangs its parts together in its own way, asks the console libraries in its own order, and makes its own
choices about timing, memory, shaders and sound. What holds here (the meeting points, the order of the flow, which
parts sit inside the weave) is not assumed to hold for another game. Each game's web has to be followed and mapped
on its own, with its own tests, the errors met along the way and the odd behaviours seen. The method carries over;
the map does not.

**One fabric, not layers next to each other.** The program, the console libraries it imports, the libraries the
installation ships beside it, its shaders, and the memory that the processor and the graphics chip both use do not
sit side by side. They sit inside each other, like the loops of a crochet: every loop holds the loops beside it.
The program's memory, its hang watchdog, its timing and its frame pacing are one system. Pull the wrong thread,
answer one call in a way the rest does not expect, and the whole fabric comes apart. Usually that happens
somewhere else, under a name that points away from the real cause.

**Where the weave reaches out.** Inside the fabric nothing is meant to be translated into something else. The
program's x64 code runs as is, the libraries shipped with it load unchanged, and their shaders are handed on as
they are. (The program's own shaders still go through an older translator; that is open work.) Only the
weave reaches out of the fabric: to Windows, to the graphics card and to the sound device. That border is the only
place where the console and the PC meet, and it is the only place this project writes code.

**Be what the program expects.** Every answer the weave gives is measured in the program first: what it calls,
with which values, and what it does with the answer. When something is missing, the weave provides that thing.
It never changes the program's own checks so that a failure looks like success, and it never answers "done" for
work that was not done. A failure stays recorded as a failure until its cause is found.

**Why this matters on a PC.** The console is close to a PC: the same kind of processor, Windows underneath,
Direct3D 12 for graphics. Most of what the program asks of its console libraries is ordinary Windows and
Direct3D 12, with a few additions. Where the PC's own libraries refuse something the program gives them (a flag,
a shader instruction, a memory kind), the fix belongs in the library at the border, so that it takes what the
program gives. The program is never bent to fit the library. That is why the weave carries its own forks of
vkd3d-proton and dxil-spirv.

So the weave is neither an emulator (no console hardware is recreated) nor a port (the program's code is not
changed or rebuilt). It is the console's side of one fabric, made for a PC, with the unmodified program woven into
it.

The measurements behind each point are in [FINDINGS.md](FINDINGS.md) and [docs/exe-map/](docs/exe-map/).

## What earlier attempts got wrong

Before this weave there was an attempt built on top of another project's runtime (Leonida, by NotRexed). None of
that code is here. The attempt failed, and the reasons are why this project is shaped the way it is. The mistakes
described are our own.

1. **It turned failures into successes.** When a memory request of the game could not be met, the runtime was
   patched so that the request came back as "done". Each patch moved the run further: a crash went away, the
   frames got brighter. The game still never reached its world, because it was now running on answers that were
   not true.
2. **It worked from inside the game instead of from outside.** The effort went into the game's own allocation
   and error paths. But the game's logic is not what is broken on a PC. What is missing is its surroundings.
   Fixing the inside hides that. (Corrected later: working from outside was not the answer
   either. The program and its surroundings are one fabric; see "What the weave is" above.)
3. **It gave the game's memory to the graphics card piece by piece.** Single regions were imported, copies were
   kept, single addresses were corrected one at a time. The game has no table for that: it expects one memory
   that both chips see. Every piece that was missed became an address the graphics card could not reach, and the
   crash at the start of the game world was exactly such an address.
4. **It measured progress by how far a run got.** A run that gets further looks like progress whether or not
   the answers behind it are right.

**Other attempts to run Xbox programs on a PC.** There are also projects, not built for one game, that run Xbox
programs on a PC, in the spirit of Wine. Their public descriptions say they provide their own, independently
written versions of the console libraries the PC lacks, and recompile the console's shaders with their own
translators (checked against their READMEs on 2026-11-27). The weave has to answer some of the same libraries. The
difference ExCorvus sees (her reading, not measured here) is that they map around the weave, while the weave
answers each call with what was measured in the executable it runs, and leaves to Windows every part Windows has.

What this project does instead, as fixed rules ([AGENTS.md](AGENTS.md)): provide what the game expects and never
change its checks; never answer "done" for work that was not done; treat the game's memory as one whole; and take
the program file itself, not the length of a run, as the reference. The longer account is in
[docs/EARLY-HISTORY.md](docs/EARLY-HISTORY.md).

## Where it stands

The weave is tested first with Grand Theft Auto VI (Xbox Series 1.0.2.1), chosen as the hardest case.

Points 1, 2, 3 and 5 work far enough that the game starts, sets itself up, opens all its packages, creates its
window and presents frames. **The window is black**: that is the game's cleared surface, because point 4 is
unfinished. The weave reads 927 of the first 1,024 shaders of a run and can write a PC shader for 496 of 505
pixel and all 227 vertex shaders, but none has been given to the graphics card yet, so every draw is held.
The exact state is the "Current state" block at the top of [FINDINGS.md](FINDINGS.md).

my own verdict. this was not written or committed by an agent. it is an impressive system that traps ai agents
exactly by the way they work. giving them instructions to "clear until intro" will make them spin out of 
control because they will inspect the code an try to fix it. 
this only a hypothesis, but the game was written in a special OS and was folded into
the xbox os. trying to stitch fix the the inside of the game will basically make the agents 
write the original os into the drive. and i tested this hypothesis with fable. but for the moment this needs
independent verification, and after further research i noticed that this stitchfix pattern shows up all over
current projects, so i think there is no way to circumvent a shell when porting these games.

## Files

```
weave/                         the runtime (C++17, MSVC); weave/README.md describes every file
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
  BUILD-AND-RUN.md             how to build, test and run the weave yourself
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
weave\build.cmd
```

Builds the weave, the console libraries and the tools, and runs the 27 tests. No game files and no GPU are needed.
To run the weave against your own copy of the game, see [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md).

## License

Copyright (C) 2026 ExCorvus. Licensed under the GNU General Public License, version 3 or later — see
[LICENSE](LICENSE) and [NOTICE.md](NOTICE.md). Game titles and related names are trademarks of their owners;
this project is not affiliated with or endorsed by them.

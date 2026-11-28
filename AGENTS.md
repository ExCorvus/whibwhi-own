# whiwhi — agent guide

Read this before changing anything. This file and [CONTRIBUTING.md](CONTRIBUTING.md) are the complete rule set.
**If something is not written in this repository, it is not a rule and not a fact about the project: ask ExCorvus
instead of assuming.**

## What this is

A shell-based weave that runs the original Grand Theft Auto VI executable (Xbox Series 1.0.2.1) on a PC. The game file is used
unmodified from the user's own copy. The weave (`weave/`, our own C++17 code) places the game in memory, answers
the console-only libraries the game imports, and is meant to show the game's memory to the PC's GPU. It is not an
emulator: no console hardware is recreated.

What works today, and where a run currently stops, is in the **Current state** block at the top of
[FINDINGS.md](FINDINGS.md). The game presents frames to its window, but none of its picture is drawn yet.

## Rules

1. **The game folder is read-only.** Never write, rename or delete anything in the user's game folder.
2. **No game data in the repository.** No executable, archives, shaders in any form (binary, disassembly,
   translated), textures, audio, or disassembly listings. Addresses inside the executable and prose about what
   the code does are fine. Before every commit run `python -I scripts/check_no_game_data.py <files>`; it must
   print `clean` and exit 0.
3. **No keys, decryption or protection workarounds. Nothing online** (no Xbox Live or Rockstar services).
4. **Host the game; do not rewrite it.** When the game fails because something it expects is missing, provide
   that thing (a library function, memory, a file path). Do not change the game's own checks so that a failure
   looks like success, and do not return success from a library function for work that was not done.
   The one place the weave changes game code is `weave/loader.cpp` (`nop_loader_stubs`): in its in-memory copy
   only, it replaces breakpoint bytes in stubs that the game's tables point at, and a breakpoint that directly
   follows an indirect call. Do not add other changes to game code without ExCorvus's decision.
5. **What is committed and what is done on the PC are two different things** (ExCorvus, 2026-11-28: "we are
   allowed to use shaders on the disc. it just needs to be a distinction between the things we commit and do").
   Committed: nothing of the game, in any form (rule 2). Done on the user's PC: the weave keeps the PC shaders it
   writes, and its refusals, in a shader store (`%LOCALAPPDATA%\whiwhi\shader-store`, one file per version of the
   translator's own code, so that a change elsewhere in the weave does not translate everything again on the
   game's clock; a second translator gets a store of its own) and loads it before the game's entry thread starts, because the time shaders take to be
   written falls on time the game's own clock already counts. The store holds the written PC shaders and, of each
   game shader, only a fingerprint (hash, length, stage), never its bytes. vkd3d's own pipeline cache is kept on
   the PC the same way (`%LOCALAPPDATA%\whiwhi\vkd3d-cache`). The game's memory is not copied to disk.
6. **Leave the game's hang watchdog alone.** The game runs its own watchdog thread (described in
   [docs/exe-map/MAP.md](docs/exe-map/MAP.md) section 4). Do not disable it and do not report progress to it that
   did not happen.
7. **One Vulkan path is banned** because it crashed the graphics driver and the whole machine (AMD RX 9070 XT,
   measured during the earlier attempt): sparse buffers created with
   `VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT`. The earlier ban on building pipelines on another thread
   was withdrawn by ExCorvus on 2026-11-26. It was not a measured crash: it came from an instruction in the earlier
   attempt to have its helpers build pipelines one at a time. The game asks for its pipelines from its worker
   threads, and the weave
   builds each one on the thread that asks, side by side (4,800 built one at a time in boots b160-b161 without a
   fault). If the driver or the machine fails during a run, write it down in FINDINGS.md before anything else.
8. **No third-party code in the repository.** vkd3d-proton (LGPL-2.1) is built separately by the user and loaded
   as a DLL; see [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md) and [NOTICE.md](NOTICE.md).
9. **The earlier attempt is not a reference.** Before this weave there was an attempt built on another project's
   runtime ([docs/EARLY-HISTORY.md](docs/EARLY-HISTORY.md)). None of its code is here. Do not reuse it, and do not
   treat what it reached as progress of this project.
10. **Measure against the game file, not against how far a run gets.** What the executable itself imports, calls
    and checks is the reference. Say what was measured and what is a guess.
11. **Every test of the weave runs on the game's own code, read fresh** (ExCorvus, 2026-11-26: "every test of the
    shell needs to run on the game's own new code, otherwise we are gonna be looping forever"). A test that says
    something about the weave takes its input from the user's game copy at the time it runs: the game's real
    shaders, files and calls, read-only and in memory. Hand-made sample programs and earlier results are not proof
    that something works. Rules 1, 2 and 5 still hold: nothing of the game is written to the repository, and on
    the PC only the shader store of rule 5. A shader taken from the store was written in an earlier run, so a run
    that takes it does not test the translator on that shader.
12. **No leaked development kits or other confidential material, not even as a reference** (ExCorvus,
    2026-11-28). Everything comes from the program itself, from Windows on the user's PC and from public
    documentation. Do not download, open or read such material, and do not take anything from texts that quote it.

## How work is done

- **Commits:** one file per commit; the message says what the file is, in plain words (the repository page shows
  the last message next to each file). Commits are pushed to GitHub as the work goes. The project is on GitHub
  only.
- **Running the game:** only the coordinating agent starts the game's entry point, and only one run at a time.
- **The game test before every code commit:** a change under `weave/` or `scripts/` is committed only after
  `scripts\game_test.ps1` has passed on that exact build (docs/BUILD-AND-RUN.md, "The game test").
- **Look it up first** (ExCorvus, 2026-11-28). Not all of the project fits into one agent's memory, and what
  falls out of it gets found again as if it were new. Before reading the executable or measuring anything, search
  [docs/INDEX.md](docs/INDEX.md), [FINDINGS.md](FINDINGS.md), `docs/` and the comments in `weave/` for it, and say
  what is already known. A larger question of that kind can go to a read-only helper agent.
- **Measure only when it has changed** (ExCorvus, 2026-11-28: "only measure when it has changed ... we cannot test
  every time"). The measurements of `measure/` run again only when what they read changed (the game folder, the
  exe, the scripts, the weave's PC graphics library); otherwise the game test takes over the last results.
- **Subagents:** allowed when ExCorvus has agreed to it. A subagent never starts the game, never uses the GPU and
  never pushes.
- **ExCorvus decides** the scope of the project and anything that changes how it is described in public.
- **Write down what you measure** in [FINDINGS.md](FINDINGS.md): a dated entry, and update the Current state
  block when the state changes.

## Where things are

- [`weave/`](weave/) — the runtime, its tools and tests; [weave/README.md](weave/README.md) lists every file.
- [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md) — how to build, test and run.
- [docs/exe-map/](docs/exe-map/) — what the executable does: memory model, watchdog, graphics calls.
- [docs/console-libs/](docs/console-libs/) — what the executable asks of each console-only library.
- [docs/SHADER-TRANSLATOR.md](docs/SHADER-TRANSLATOR.md) — the shader translation plan.
- [FINDINGS.md](FINDINGS.md) — current state and the dated log of measurements.
- [`measure/`](measure/) — scripts that repeat the measurements on anyone's own copy, read-only
  ([measure/README.md](measure/README.md)). Not part of the build or the game test. A result read from the
  executable gets a script here that checks it.
- [ATTRIBUTION.md](ATTRIBUTION.md), [NOTICE.md](NOTICE.md), [LICENSE](LICENSE) — credit and licence.

## Build and test

```
weave\build.cmd
```

Builds the weave, the console libraries and the tools, and runs the 27 tests. No game files and no GPU are needed
for them. They check pieces on hand-made sample programs and are not proof that the weave works with the game
(rule 11): that is tested only on the game's own code, read from the user's copy when the test runs.

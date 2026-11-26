# whiwhi — agent guide

Read this before changing anything. This file and [CONTRIBUTING.md](CONTRIBUTING.md) are the complete rule set.
**If something is not written in this repository, it is not a rule and not a fact about the project: ask ExCorvus
instead of assuming.**

## What this is

A shell that runs the original Grand Theft Auto VI executable (Xbox Series 1.0.2.1) on a PC. The game file is used
unmodified from the user's own copy. The shell (`shell/`, our own C++17 code) places the game in memory, answers
the console-only libraries the game imports, and is meant to show the game's memory to the PC's GPU. It is not an
emulator: no console hardware is recreated.

What works today, and where a run currently stops, is in the **Current state** block at the top of
[FINDINGS.md](FINDINGS.md). Nothing has been drawn on screen yet.

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
   The one place the shell changes game code is `shell/loader.cpp` (`nop_loader_stubs`): in its in-memory copy
   only, it replaces breakpoint bytes in stubs that the game's tables point at, and a breakpoint that directly
   follows an indirect call. Do not add other changes to game code without ExCorvus's decision.
5. **Keep it in memory.** Do not copy the game's memory or its whole shader set to disk. Shaders are translated
   when the game asks for them, and kept in memory.
6. **Leave the game's hang watchdog alone.** The game runs its own watchdog thread (described in
   [docs/exe-map/MAP.md](docs/exe-map/MAP.md) section 4). Do not disable it and do not report progress to it that
   did not happen.
7. **Two Vulkan paths are banned** because they crashed the graphics driver and the whole machine (AMD RX 9070 XT,
   measured during the earlier attempt): sparse buffers created with
   `VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT`, and building pipelines on another thread.
8. **No third-party code in the repository.** vkd3d-proton (LGPL-2.1) is built separately by the user and loaded
   as a DLL; see [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md) and [NOTICE.md](NOTICE.md).
9. **The earlier attempt is not a reference.** Before this shell there was an attempt built on another project's
   runtime ([docs/EARLY-HISTORY.md](docs/EARLY-HISTORY.md)). None of its code is here. Do not reuse it, and do not
   treat what it reached as progress of this project.
10. **Measure against the game file, not against how far a run gets.** What the executable itself imports, calls
    and checks is the reference. Say what was measured and what is a guess.

## How work is done

- **Commits:** one file per commit; the message says what the file is, in plain words (the repository page shows
  the last message next to each file). Commits are pushed to GitHub as the work goes. The project is on GitHub
  only.
- **Running the game:** only the coordinating agent starts the game's entry point, and only one run at a time.
- **Subagents:** allowed when ExCorvus has agreed to it. A subagent never starts the game, never uses the GPU and
  never pushes.
- **ExCorvus decides** the scope of the project and anything that changes how it is described in public.
- **Write down what you measure** in [FINDINGS.md](FINDINGS.md): a dated entry, and update the Current state
  block when the state changes.

## Where things are

- [`shell/`](shell/) — the runtime, its tools and tests; [shell/README.md](shell/README.md) lists every file.
- [docs/BUILD-AND-RUN.md](docs/BUILD-AND-RUN.md) — how to build, test and run.
- [docs/exe-map/](docs/exe-map/) — what the executable does: memory model, watchdog, graphics calls.
- [docs/console-libs/](docs/console-libs/) — what the executable asks of each console-only library.
- [docs/SHADER-TRANSLATOR.md](docs/SHADER-TRANSLATOR.md) — the shader translation plan.
- [FINDINGS.md](FINDINGS.md) — current state and the dated log of measurements.
- [ATTRIBUTION.md](ATTRIBUTION.md), [NOTICE.md](NOTICE.md), [LICENSE](LICENSE) — credit and licence.

## Build and test

```
shell\build.cmd
```

Builds the shell, the console libraries and the tools, and runs the 15 tests. No game files and no GPU are needed.

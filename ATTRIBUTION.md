# Attribution

This file records which insights were **ExCorvus's** and which implementation work was the **agent's**, so the credit
is honest. The project description is in [README.md](README.md); the required credit terms are in
[NOTICE.md](NOTICE.md).

**This file is a record of credit, not a list of rules.** The rules are in [AGENTS.md](AGENTS.md). Several items
below come from the earlier attempt ([docs/EARLY-HISTORY.md](docs/EARLY-HISTORY.md)), whose code is not in this
repository; they are kept because the ideas were ExCorvus's, not because they describe what the shell does today.

## ExCorvus's ideas (the direction)

0. **Where the idea came from.** ExCorvus got the idea for the shell by inspecting the game itself, the Xbox
   Series build of GTA VI: its code is ordinary 64-bit code a PC processor runs as is, and what it lacks on a PC
   is what surrounds it on the console. Everything below grew from that.
1. **The watchdog ground truth.** The watchdog is a small parallel execution folded into the game executable,
   running alongside the game and checking against it (framerate, and the post-boot user environment variable).
   It stays armed; it is ground truth, not a thing to disable or probe.
2. **The AI trap — outward, not from within.** The trap is looking at the code from within instead of outward:
   "fixing" the game's allocation/refusal/error paths into success rather than observing what the game does from
   outside and hosting the environment it expects. The project is a shell that hosts the game, not an emulator.
   "When you guys inspect code you fix it instead of keeping the scope." (Corrected by item 14: outward is not
   the answer either.)
3. **The Xbox APU / unified-memory insight.** The Xbox shares RAM and VRAM; some addresses the game uses belong to
   system RAM, which a discrete PC GPU cannot see unless they are shared.
4. **The game's own code adjusts the heaps.** The cleaner path is to host the game's own heap-adjusting code rather
   than paper over it: provide the environment, let the game's own logic run.
5. **Rendering + pipeline timing + the fps cap are one system.** The frame-event cadence is the fps cap, and the
   GPU-driven mesh rendering runs inside that frame.
6. **The fps cap is baked into pipeline timing, not physics** (unlike Bloodborne), so framerate adjustment is
   feasible later — after the world renders; pacing stays intact for now.
7. **Build our mod from scratch.** Do not use the upstream mesh replay; it is faulty.
8. **A full installation on D: (NVMe)** for closest-to-real conditions; remove the scattered test exes.
9. **Software-reset the graphics driver after every boot** so a broken driver state does not force a PC restart.
10. **The remaining odd addresses might be CPU-cache aliases** of the same physical memory.
11. **Commit our findings to our own git and relay the attribution** (this file).
12. **The sources are the original game files; the port is completely new and independent.** A fresh port of the
    original executable; a shell, not an emulator.
13. **One singular piece of software — a shell that wires the game into it.** The game is mapped into the shell,
    its imports resolved to the shell's shims, its memory hosted by the shell; the whole is one program.
14. **A weave, not a wrapper (the latest find, 2026-11-26).** By its code the shell is the same as a shell, but it
    is not wrapped around the executable: it is woven all through it. Its entry point is neither outside the code
    nor inside it, but in the middle, like a crochet: every loop holds the loops beside it, and pulling the wrong
    thread makes the whole system collapse. This corrects item 2: looking from outside was not the answer either.
    The layers are inside each other, not next to each other: the program, the console libraries it imports, the
    libraries shipped beside it, the shaders and the memory the CPU and the GPU both use are one fabric, and only
    the shell reaches out of it (to Windows, the graphics card, the sound device). So no piece can be read or
    changed on its own: a change to one loop is a change to the fabric. ExCorvus proposes to call the project a
    shell-based weave.
    An example from the same day: the program starts the installation's own ray-tracing builder; the builder's
    shaders call console functions (named `xbox.`) that are in none of the shipped files, because the console's
    graphics library supplies them at the moment it builds those shaders - and the result runs on the GPU in
    memory the CPU side uses too. One thread runs through the program, a shipped library, the graphics library
    and the GPU.

## The agent's work (the implementation)

In this repository:

- **Mapped the original executable** (read-only, our own tooling): memory start-up, the streaming page pool, the
  page mapper (queue slot 31), the watchdog and its callees, the fatal-stop routine, the buddy allocator, the
  graphics calls and the console libraries. See `docs/exe-map/` and `docs/console-libs/`.
- **Wrote the shell** in `shell/`: the image reader and loader, thread-local storage for the game, the console
  libraries, the tools and the tests. Helper agents mapped and wrote the first versions of the audio, file-read
  and graphics-call pieces and the shader translator groundwork; the coordinating agent reviewed and merged them.

In the earlier attempt (code not in this repository):

- **Built the fault-trace diagnostic** (`FAULT_TRACE`): at a GPU fault it reports what the faulting address is
  (host state, GPU/CPU view, pool, alias, shadow tracker), which descriptor slots name it, and where the address is
  stored in process memory.
- **Memory-exposure attempts (superseded).** Linear-pool host import, contiguous GPU-view, all-channel chunks,
  committed-RAM import and the region-anchor fix. These cleared the world-start write fault and brightened frames
  but never reached the world; they are heap patches, superseded by the flat hosting model written from the exe map.
- **Mesh-indirect replay** (built from scratch per ExCorvus's directive; reviewed, folded, validated):
  `ExecuteIndirect(DISPATCH_MESH)` records a mesh draw instead of dropping it.
- **Install mode fix** (apply the runtime defaults `--run` skips), **BCPack/BVH calibration copy**,
  **driver-reset automation + preflight** in the boot runner.
- **Corrections of its own errors:** the "0 descriptors rebased" claim was a misread of a resetting counter
  (~100k actually rebased); boot 23's crash was a broken driver state, not the committed import (the untouched
  plain build failed the same way).
- Ran and analysed the boot series, the standalone GPU capability probes, and the test suites.

## Shared / verified facts

- The game keeps one linear pool of 64 KiB pages, shown in a CPU view and a GPU-only view; there is no
  address-conversion table — the page mapping is the whole mechanism.
- In the earlier attempt, the crash at the start of the game world was a write through an address the GPU could
  not reach; the mesh draws were also dropped.
- Sparse `DEVICE_ADDRESS_CAPTURE_REPLAY` buffers destabilised the AMD driver; they are banned. Ordinary
  (non-sparse) host imports are safe.
- Pipelines built on the threads that ask for them, side by side, are allowed (ExCorvus, 2026-11-26). The earlier
  ban on that came from an instruction in the earlier attempt to build them one at a time, not from a measured
  crash; the shell has built tens of thousands of pipelines this way without a fault (boots b163 and later).

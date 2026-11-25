# Attribution

This file records which insights were **ExCorvus's** and which implementation work was the **agent's**, so the credit
is honest. The project description is in [README.md](README.md); the required credit terms are in
[NOTICE.md](NOTICE.md).

## ExCorvus's ideas (the direction)

1. **The watchdog ground truth.** The watchdog is a small parallel execution folded into the game executable,
   running alongside the game and checking against it (framerate, and the post-boot user environment variable).
   It stays armed; it is ground truth, not a thing to disable or probe.
2. **The AI trap — outward, not from within.** The trap is looking at the code from within instead of outward:
   "fixing" the game's allocation/refusal/error paths into success rather than observing what the game does from
   outside and hosting the environment it expects. The project is a shell that hosts the game, not an emulator.
   "When you guys inspect code you fix it instead of keeping the scope."
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

## The agent's work (the implementation)

- **Mapped the original executable** (read-only, our own tooling): memory start-up, the streaming page pool, the
  page mapper (queue slot 31), the watchdog and its callees, the fatal-stop routine, the buddy allocator. See
  `docs/exe-map/MAP.md` and `docs/exe-map/CHANNELS.md`.
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
- The world-start crash was an untranslated write descriptor in committed placed RAM; the mesh draws were also
  dropped.
- Sparse `DEVICE_ADDRESS_CAPTURE_REPLAY` and off-thread (async) pipeline builds destabilised the AMD driver; they
  are banned. Ordinary (non-sparse) host imports are safe.

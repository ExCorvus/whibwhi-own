# Findings — GTA VI Xbox 1.0.2.1 → PC (our own port)

Private working notes. No game bytes/offsets here; RVAs appear only as named constants already in our
source. See `docs/exe-map/MAP.md` (the executable map) and `docs/exe-map/CHANNELS.md` (every channel that
hands a heap address to the GPU). Attribution of ideas vs implementation: `ATTRIBUTION.md`.

## The footing (ExCorvus, 2026-11-25): a shell — outward, not from within

The entire project is a **shell** that hosts the original exe (not an emulator; we do not recreate hardware). The
AI trap is looking at the code **from within instead of outward**: patching the game's allocations/refusals into
success (the heap "fixes") vs observing what the game does from outside and hosting the exact environment it
expects. The heap-fix line (committed-RAM host imports, region-anchor, contiguous coverage) is **stopped** as
trap-1; it advanced the boot but never reached the world. Next: map the exe's real heap/memory setup and write our
own hosting model from it. See `docs/EARLY-HISTORY.md`. Going outside the game's scope is allowed when it serves
the porting project (host/guide via mapped memory); the trap is fixing the game's own logic, not touching its memory.

## The memory model (verified in the exe)

- At start-up the game reserves two 24 GiB views: a CPU-mappable one and a GPU-only one (paired).
- It keeps **one linear pool of 64 KiB pages** (~6 GiB, 99,776 pages) and shows pool pages in the views:
  - CPU view via `XMemMapPhysicalPagesScatter`,
  - GPU-only view via console queue **slot 31** (`CopyPageMappingsBatchX`), one pool page per view page.
- **There is no address-conversion table.** A view page number means the same page in both views; the page
  mapping itself is the whole mechanism. A GPU address is valid exactly when a pool page has been mapped there.
- The game also places buffers/textures at its own addresses (device slot 133) and commits fixed pools
  (`XMemVirtualAlloc`, often `PAGE_READWRITE | PAGE_NOCACHE`). These allocations mix committed-private runs
  with mapped/reserved **holes**.

## Why the PC GPU faulted

A discrete PC GPU does not share the Xbox APU's unified memory. The game's shaders compute
`heap_base + address` and follow raw title addresses (descriptor V#s and pointers embedded in buffers).
Where the layer had not made that memory GPU-visible, the GPU read/wrote a dead address → page fault.

- ~100,000 descriptors were translated correctly; ~25,000 were left "waiting".
- The world-start crash was an **untranslated WRITE descriptor** in committed placed RAM: the page had an
  arena mirror, but the shader used the raw title address. A read-only mirror cannot accept a write, so the
  fix is to share the game's actual committed page (writable host import).
- The committed import initially refused these allocations because it walked from the allocation base and
  stopped at the first mapped/reserved hole before the target page. **Fix:** anchor the extent at the page's
  own homogeneous `VirtualQuery` region and extend forward through adjacent same-allocation runs.

## The rendering / timing / fps system (one system)

- The fps cap **is** the frame-event cadence: `SetFrameIntervalX` / `ScheduleFrameEventX` /
  `WaitFrameEventX` / `PresentX`. The loop waits on a frame event, submits GPU work, presents.
- The world is GPU-driven instanced mesh via `ExecuteIndirect(DISPATCH_MESH)`; thread-group counts and
  per-instance data come from buffers the game's compute fills each frame. These must be GPU-resident.
- `ExecuteIndirect(DISPATCH_MESH)` was dropped by the replay; it is now recorded as a mesh draw
  (`vkCmdDrawMeshTasksIndirect(Count)EXT`), built from scratch (not the upstream replay).
- Amplification/task shaders remain a separate gap.

## Driver stability (hard-won)

- Sparse buffers with `VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT` and off-thread (async) pipeline
  builds **reset the AMD driver / whole system**. Both are banned.
- Ordinary (non-sparse) host imports of committed memory are safe.
- The driver can also enter a state where it stops reading imported host memory at all (every build then
  dies at ~11 s, even the untouched plain one). A one-second capability test (`vk_host_region_import_test`)
  detects it; the boot runner runs it as a preflight and software-resets the driver after each boot.

## Status

Intro / loading / story scenes render. The mesh draws are delivered and the committed-residency fix is in.
Validation of the world frame is ongoing. Acceptance remains: actually in the playable world.

## Update (helper findings, folded)

- Mesh-indirect replay (from scratch): ExecuteIndirect(DISPATCH_MESH) recorded as a mesh draw; dropped lists 12 -> 0. See docs/reports/mesh-indirect-report.md.
- Committed-RAM residency: extent anchored at the page's own region; and a contiguous title chunk now covers committed spans that cross region holes (no capture/replay). See docs/reports/committed-contig-report.md.
- Boot 32 cleared the world-start WRITE fault; a hole-crossing READ span remained, addressed by the contiguous chunk. Validation ongoing.

## Watchdog relay (mapped + implemented)

The watchdog is the liveness relay of the loop that paces the frame: a time-critical thread (fn 0x2215a60) that watches the run loop's progress reporter (fn 0x22159f0 = QPC store). A long block in our shim is a 30 s gap -> fire. It does NOT drive the frame events/present directly. Implemented: a relay that calls the game's own reporter only while our layer holds the title thread, so the watchdog stays armed but survives our compiles. See docs/reports/watchdog-relay-report.md and docs/exe-map/MAP.md section 4.

## The shell footing (ExCorvus)

The runtime is a SHELL hosting a game that wants the shell to keep it alive. The watchdog is the game's heartbeat check on the shell; the heaps and frame cadence are what it expects the shell to provide. The AI trap is treating this as 'fix the game' instead of 'be the shell it expects'.

Boot 35 (watchdog ARMED + liveness relay): no watchdog fire, no GPU fault, no device loss; frames render and brighten. The shell keeps the game alive. Remaining gap: BVH ray-tracing rehydration (E_NOTIMPL).

## Live status (kept current)

- Boot 35 (watchdog ARMED + liveness relay, full memory+mesh system): **no watchdog fire, no GPU fault, no device loss**; frames render and brighten. The shell keeps the game alive.
- World stops at BVH ray-tracing rehydration (`RehydrateRaytracingAccelerationStructureBatched2X` E_NOTIMPL). Implementation in progress.
- Earlier: boot 32 cleared the world-start WRITE fault (committed-memory region-anchor fix); a hole-crossing READ span remains (contiguous coverage being reworked to avoid over-import).
- Driver safety: preflight host-import check + conditional driver reset after each boot; sparse capture/replay and async pipeline builds remain banned.

- BVH ray-tracing rehydration implemented (rebuild from element geometry via the XDXR builder; BOX16; size-guarded). Boot 36 verifying. See docs/reports/bvh-rehydrate-report.md.

- Shell footing refined (ExCorvus): the shell GUIDES the game � it is the active host driving frame cadence, memory and liveness, not a passive reverse-and-check harness. The shell leads; the game follows.

- Boot 36: BVH ray-tracing rehydration WORKS (rebuilt by the layer's builder); watchdog armed satisfied; frames render brighter. Remaining: a hole-crossing READ descriptor in committed placed RAM (the contiguous coverage must be lazy/on-demand to avoid over-import).

## 2026-11-25 — the scrape + the port begins (our own code only)

- ExCorvus's directive: scrape everything from Leonida; **no upstream code enters the project**. The only source is our
  own files; only the credit for the idea stays, with our runtime. It is a **shell based on the original EXE, nothing
  else**. No more heap conversion.
- Removed all Leonida source from whiwhi (runtime/, tools/, tests/, third_party/, viewer/, the upstream scripts and
  docs, make_game.py). The Leonida tree stays safe in E:\Leonida as referenced prior art; none of it is vendored here.
- Started our own runtime (shell/): the original-exe image mapper (image.*) + the flat-out zero-copy hosting model
  (hosting.*) + our own tests. Builds and both tests pass (shell\build.cmd). The flat-out model resolves hole-crossing
  spans directly (the boot-36 fix) — no piecemeal import, no heap conversion.
- Our own no-game-data gate (scripts/check_no_game_data.py) replaces the upstream one.

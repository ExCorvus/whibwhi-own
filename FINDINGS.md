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

- Shell footing refined (ExCorvus): the shell GUIDES the game - it is the active host driving frame cadence, memory and liveness, not a passive reverse-and-check harness. The shell leads; the game follows.

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

## 2026-11-26 - what the original executable imports (measured)

- The import table of the original executable lists 68 libraries and 587 functions, 24 of them by ordinal; it has
  no delay-load imports. Two independent readers agree on these numbers: our import table reader
  (shell/imports.*, run through shell/tools/list_imports.cpp) and a separate script.
- Seven of the libraries exist only on the console and are not shipped with the game: xmem.dll (12 functions),
  AcpHal.dll (9), xapu.dll (5), chad.dll (5), d3d12_xs.dll (3), xg_xs.dll (2), XCRDAPI.dll (1) - 37 functions in
  all. These are what the shell has to provide itself. chad.dll and XCRDAPI.dll are not yet identified.
- The graphics library is only 3 imported entry points, but the whole graphics interface is reached through them,
  so that number understates the work.
- Progress is measured against this list (what the executable itself declares it needs), not by how far a run gets.

## 2026-11-26 - what the original executable asks of a loader, and what connects on a PC (measured)

- Loader facts read from the file (shell/tools/describe_image.cpp): 176.4 MiB in memory in 10 sections; it has a
  preferred address and is also movable (598,282 relocation entries, all 64-bit); 299,257 exception-table entries;
  thread-local storage of 5,248 bytes with 2 callbacks; no delay-load imports; no control-flow guard.
- Our loader (shell/loader.*) places it without starting it: it lands at its preferred address, so none of the
  relocations are needed; all 299,257 exception-table entries register.
- Connecting imports to the same-named libraries on the PC and in the game folder (shell/host_libraries.*, run
  through shell/tools/connect_imports.cpp): 520 of 587 functions connect. 67 do not, in 9 libraries: the 7
  console-only libraries (37 functions) plus two libraries that ship with the game but do not load on a PC,
  Bink2Scarlett.dll (23) and XboxBvhBuilder.dll (7).
- Why those two do not load: each depends on console-only libraries itself. Bink2Scarlett.dll needs xmem.dll;
  XboxBvhBuilder.dll needs xmem.dll and d3d12_xs.dll. Once the shell provides those two as real libraries, both
  should load unchanged (expected, not yet shown).
- Every import that is not connected points at a stub that names the function if the game calls it.
- Not done yet: thread-local storage for the placed image, and starting it.
- The game folder listing was identical before and after (nothing written).

## 2026-11-26 - thread-local storage for the placed image (measured)

- The original executable asks for 5,248 bytes of initial thread-local data per thread, no zero-filled bytes,
  16-byte alignment, and has 2 thread-storage callbacks (shell/tools/describe_image.cpp).
- Windows gives thread-local storage only to libraries its own loader loaded, not to an image the shell placed.
  The shell therefore ships a small library of its own, whiwhi_tls.dll (shell/thread_block.*), whose thread-local
  section is one empty 64 KiB block, and lends that block to the image: the loader writes the library's storage
  index where the image's code reads it, and every thread gets the image's initial data at the start of its
  block. One loaded image can hold the block at a time.
- Measured on Windows build 26300 while building it:
  - A library that has a thread-local storage directory but imports nothing gets no storage: Windows ignores the
    directory and leaves the index unwritten. With a single import it gets an index and a block per thread. The
    library keeps one import for this reason and reports "no index" instead of using another library's block.
  - Windows honours the alignment the directory declares. Over 60 threads, with 64 bytes declared all 60 blocks
    were 64-byte aligned; with 16 declared only 3 to 6 of 60 happened to be. The library declares 64.
- Tested with a synthetic image (shell/tests/test_thread_storage.cpp) whose own machine code reads its
  thread-local data the way compiled code does: correct on the loading thread, on threads started afterwards, on
  a thread that was already running (after it asks for the data), and with the image moved from its preferred
  address. Threads do not see each other's changes. The callbacks are not called until the image is started,
  then once for the start and for every thread that starts or ends. An image asking for more bytes or more
  alignment than the block has is refused with a reason.
- On the original executable (loaded without starting it, shell/tools/connect_imports.cpp): 5,248 bytes per thread
  in storage index 1; the 2 callbacks were not called. Imports unchanged at 520 of 587. The game folder listing was
  identical before and after.
- Not done yet: the console-only libraries, and starting the image.

## 2026-11-26 - the memory library, xmem.dll (our own; measured)

- What an image asks of this library was read off the original executable's own calls: 57 call sites in 21
  functions. Microsoft's reference pages for the library are not public and were not used. The flag values the
  executable passes are named in the public Windows SDK header winnt.h: MEM_64K_PAGES (0x20400000),
  MEM_LARGE_PAGES (0x20000000), MEM_RESERVE_PLACEHOLDER (0x40000), MEM_REPLACE_PLACEHOLDER (0x4000),
  PAGE_WRITECOMBINE (0x400) and the PAGE_GRAPHICS_* protections (0x800 to 0x40000). Earlier notes called 0x400
  "no cache"; that was wrong.
- The executable uses memory in four ways:
  1. It reserves a range for 64 KiB pages, asks for physical pages, and maps them in page by page: as a run from
     one address, or as a list of separate addresses. An entry of 0, or no entry list, unmaps.
  2. It reserves one placeholder range, splits it itself with VirtualFree, and turns each piece into a range for
     pages. This is the same placeholder scheme Windows has.
  3. It commits memory outright, in large-page or 64 KiB-page mode.
  4. It makes a few plain allocations with XMemAlloc and frees them with XMemFree.
- It frees what XMemVirtualAlloc gave it with Windows' VirtualFree (23 call sites), in one place passing the
  region's size where Windows wants 0. Windows refuses that call (measured: error 87).
- Our xmem.dll (shell/xmem.*) answers 9 of the 12 functions: XMemVirtualAlloc, XMemAllocatePhysicalPages,
  XMemFreePhysicalPages, XMemMapPhysicalPages, XMemMapPhysicalPagesScatter, XMemAlloc, XMemFree, XMemAllocDefault
  and XMemFreeDefault. The shell connects the executable's VirtualFree import to the library as well
  (shell/console.*), because only the library knows how its regions are built.
- Left out on purpose: XMemGetWorkingSetStatistics, XMemIsToolsMemAvailable and XMemTransferMemory. The calls show
  their arguments but not what the results mean, so they stay "not provided" and name themselves if called.
- How it sits on Windows, measured with a probe before it was written (Windows build 26300):
  - Splitting off a range that is a whole placeholder is refused (error 487), so the library keeps a record of
    every placeholder. Many adjacent placeholders merge in one call.
  - One page mapped at two addresses is the same memory at both.
  - A native release of a range that has been split releases only its first piece, and VirtualFree on a mapped
    page is refused (error 87). This is why the executable's VirtualFree has to be answered by the library.
  - 16,384 single-page mappings (1 GiB) took 45 ms to map and 33 ms to unmap. Each page is therefore its own
    mapping, so changing one page never disturbs a neighbour.
- Choices that are ours and not known console behaviour: pages read as zero when handed out; an allocation of
  pages is all or nothing; XMemAlloc memory is zero-filled and aligned to 64 KiB; write-combining is recorded but
  not applied (it is a caching hint); "large pages" are ordinary pages aligned to 2 MiB. A combination of flags
  the executable was not seen to use is refused and reported, not guessed. The graphics bits and attributes of
  every region are kept for the shell; nothing shows them to the GPU yet.
- No file is written: physical pages are pagefile-backed memory, created 256 MiB at a time as they are asked for.
- Tested with made-up sizes only (shell/tests/test_xmem.cpp, shell/tests/test_console.cpp): mapping, unmapping,
  the same page at two addresses, freed pages coming back as zero, the placeholder sequence above with the split
  done through the library and directly through Windows, release with pages still mapped, four threads changing
  one range at once, 5,000 pages across two backing sections. 41 runs in a row passed.
- On the original executable (loaded without starting it): 552 of 587 imports connect, 10 of them to our library
  (the 9 functions and VirtualFree). Bink2Scarlett.dll now loads unchanged against our library; its start-up code
  made no call to it. 35 imports are not provided: the 3 above, and 32 in 7 libraries that do not load:
  AcpHal.dll (9), XboxBvhBuilder.dll (7), chad.dll (5), xapu.dll (5), d3d12_xs.dll (3), xg_xs.dll (2),
  XCRDAPI.dll (1). XboxBvhBuilder.dll also asks our library for XMemVirtualQuery, a function the executable itself
  does not import. chad.dll is five XDsp functions and XCRDAPI.dll is DStorageGetFactory. The game folder listing
  was identical before and after.
- Not done yet: the remaining console libraries, showing the regions to the GPU, and starting the image.
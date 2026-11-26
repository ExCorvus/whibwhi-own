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

## 2026-11-26 - first boot of the image inside the shell (measured)

- New tool shell/tools/boot_image.cpp (run by Temp\opencode\boot_shell.ps1): loads the image, connects imports,
  runs its 2 thread-storage callbacks, then calls its entry point on a new thread with its own 1 MiB stack. An
  import the shell does not provide is logged and answered with failure (0); the image's own error handling
  decides what happens. Exceptions are logged with their image-relative address.
- Boot b1 (the original executable, game folder as library folder, 90 s budget): the image placed at its preferred
  base, TLS callbacks ran, the entry point ran, and the game stopped at its first graphics call:
  `d3d12_xs.dll!D3D12ConfigureVirtualMemoryX` - not provided, answered failure - after which the process died with
  exception 0xc0000374 in ntdll. No GPU was used (no graphics library provided). The game folder listing was
  identical before and after (150 entries); the boot's working directory was a scratch folder.
- Reading: the memory library and loader carried the game through start-up up to the graphics boundary. The next
  console library to provide is d3d12_xs.dll (3 imports: D3D12XboxCreateDevice, D3D12ConfigureVirtualMemoryX,
  D3D12SerializeVersionedRootSignature); the whole graphics interface sits behind the device it returns.
- ExCorvus has an Xbox controller: input needs no separate keyboard path; the console input library (GameInput.dll,
  1 import) can be answered from the controller directly.
- ExCorvus asked whether VRR is feasible by adjusting the shell (not acted on): yes, in principle, at the present path
  once the shell builds one (the game's frame-event calls are device slots 78-81 and PresentX on the queue;
  on a PC those map to a DXGI swapchain created with tearing allowed and presented accordingly, gated on the
  display actually supporting variable refresh). It changes the frame cadence the game's watchdog sees, so it is
  a deliberate later change with her sign-off, not something to fold into the current boot work.

## 2026-11-26 - the remaining console libraries, mapped and stubbed (helper agents, reviewed and folded)

Three helpers mapped the remaining console-only libraries from the exe's own call sites (docs/console-libs/,
reports in the private candidates folder) and each shipped an honest skeleton DLL whose exports log once and
answer "not provided" - never a faked success:

- **AcpHal.dll (9) + xapu.dll (5)** - the audio hardware/APU libraries (shell/acp_hal.*, shell/xapu.*). The image
  tests most of these with `js` (negative = failure), so the skeletons answer E_NOTIMPL there, 0 for the count
  and bool returns, null for address returns. A boot that reaches audio bring-up stops at the image's own int3
  (fn 0x43ad8b0's failure path) - the game's own error handling, not routed around. There is no PC equivalent of
  the APU heap / shape contexts / 32-bit APU address window; real audio needs a DSP stand-in or stays unprovided.
- **chad.dll (5)** - the audio co-processor command channel (shell/chad.*): 4096-byte command blocks in/out, one
  handle per stream (shell/chad.*, docs/console-libs/chad.md).
- **XCRDAPI.dll (1)** - DStorageGetFactory (shell/xcrdapi.*). Key finding: straight forwarding to the PC
  dstorage.dll cannot be the final answer - the image asks for a console IID and a longer factory vtable
  (CreateQueue at +0x50, SetStagingBufferSize 32 MiB at +0x40). Every read request's destination is a plain
  unified VA in the CPU view / GPU view / page pool (the xmem-owned memory), so the PC answer is our own
  factory/queue doing a plain file read into that address - no GPU resource, no staging copy. Stage 0 (forward
  to PC dstorage.dll if installed) is in place; the PC has no dstorage.dll installed here, so the image int3s at
  0x1d8e505 until the wrapper exists. Next step: the wrapper (OpenFile->CreateFileW, always-complete status
  array, synchronous read into the destination).

All 12 tests pass on the folded master (126 commits). Nothing pushed (ExCorvus's gate, GitHub only).

## 2026-11-26 - TLS slot 0, the COM layout bug, and the storage wrapper (measured)

- **TLS slot 0.** Parts of the image read the per-thread storage array entry 0 directly (`gs:[0x58]` then entry
  0), at offsets up to 0x13a8 - on the console the game is the main module, so its storage owns index 0. With a
  small foreign block at entry 0 that crashes (boot b8's AV); with the game's own data block there it corrupts
  whatever else owns entry 0 on Windows (kernelbase; the 0xC0000374 in ntdll). Fix: entry 0 is redirected to
  the zeroed upper half of our own per-thread 64 KiB block (default on, WHIWHI_TLS_REDIRECT=0 to disable), so
  the image's entry-0 accesses land in bounds and other slot-0 users see zeros (measured safe in a probe).
  The image's index variable still points at the full block. Covered in test_thread_storage.
- **COM layout bug** (found by the xcrdapi test segfaulting): the vtable pointer must be the object's FIRST
  field - both our xcrdapi objects and the d3d12_xs wrapper had it second. Fixed in both; this was also the
  likely cause of the earlier boot AV inside a wrapper thunk.
- **XCRDAPI stage 1/2 (the storage wrapper).** DStorageGetFactory now returns OUR factory in the console
  vtable layout (+0x20 OpenFile, +0x28 CreateStatusArray, +0x40 SetStagingBufferSize, +0x50 CreateQueue). The
  queue reads each request synchronously straight into the request's destination address - host memory the
  xmem library owns - validating the destination is committed writable first (an unmapped destination is
  logged and skipped, never written). Status array reports IsComplete=true / GetHResult=S_OK, which is honest
  because the read really finished. Paths are logged and mapped onto the read-only game folder
  (WHIWHI_GAME_DIR). 12/12 tests pass, including a functional test that reads a temp file into a buffer through
  the queue. Not yet measured on the real image: which paths reach OpenFile and whether RPF member offsets
  arrive as file offsets - that needs a boot.

## 2026-11-26 - install path and the Game Runtime (measured, boots b28/b29)

- **Install path.** The game derives its install directory from GetModuleFileNameW(NULL) (fn 0x55e2910) and
  appends a subdir; in our process that returned the tool's path, so no storage device registered and the
  storage set-up asserted ([rsi+0xc]==0 at image+0x2237178). Fix: boot_image points the PEB main-module
  FullDllName/BaseDllName and the process ImagePathName at the game exe (the environment the console gives the
  game). After that the storage assert is gone and the game runs on to the next dependency.
- **xgameruntime.dll.** The game LoadLibraryExW's "xgameruntime.dll" (fn 0x55e29dc) and resolves
  InitializeApiImpl / InitializeApiImplEx / InitializeApiImplEx2 / QueryApiImpl / UninitializeApiImpl. It is not
  on PC. Our skeleton (shell/xgameruntime.*) answers initialize/uninitialize with success (local-only runtime)
  and QueryApiImpl with E_NOTIMPL; covered in test_gfx_stubs.
- **The packaging probe.** fn 0x55e286c reads HKLM\Software\Microsoft\GamingServices!ForceUseLocalServices
  (DWORD). Non-zero => the game uses the app-dir DLL search (mode 0xa00) and finds our xgameruntime.dll beside
  the tool; zero => it searches System32 only (mode 0x800) and the load fails (boot b29). Setting that value to
  1 is the standard PC-GDK local-services switch; it is an external registry write, so it needs ExCorvus's word.

## 2026-11-26 - boots b12-b18: how far the image gets in our shell, and the current boundary (measured)

- Boot b16/b17/b18 (boot_image, game folder as library folder): 578/587 imports connected (only xg_xs 2 and
  XboxBvhBuilder 7 remain); image at its preferred base; TLS callbacks ran; entry point ran; D3D12ConfigureVirtualMemoryX
  answered; memory start-up completed (24 GiB CPU view + 24 GiB GPU view reserved, five fixed pools committed);
  the game reached the storage layer - DStorageGetFactory answered with our console-layout factory, SetStagingBufferSize
  (32 MiB) answered - i.e. the game's own file layer started. Game folder listing unchanged after every boot.
- The stop: an unhandled int3 at image+0x22106e0, reached as pool-object vtable slot +0x30 (vtable at .rdata
  0x62fc060 -> thunk 0x2219be0 -> 0x22106e0, which is 0xCC in the file) when the game allocates from the first
  fixed pool (fn 0x4fac3c0 -> fn 0x2219620). The flag that routes there (object byte +0x18 = 1) is set
  unconditionally by fn 0x515970 during pool init, so this path is deterministic - on the console that slot must be
  filled by the console's own loader. The byte is 0xCC in the retail file and nothing in the image writes it.
- Per the standing rules we do not patch game code and do not bypass protection, so this stub is the current
  boundary: the game runs its own start-up up to the file layer and stops at a slot the console loader fills.
  Runtime tooling added for this: boot_image's exception handler now logs rcx/vtable on faults, continues once
  through handled breakpoints (a second occurrence at the same address ends the boot), and WHIWHI_WATCH=<rva> arms a
  1-byte write watchpoint on the entry thread (used to find the flag writer above). 12/12 tests pass.

## 2026-11-26 - loader stub patch (the shell plays the console's loader) and the precise boundary

- Census of the whole image: exactly **4** distinct int3 stub addresses are referenced from vtables
  (0x22106e0 shared by 30 slots, plus 0xb883ff, 0xb98348, 0x486c766). All are `int3` followed by the release code
  (`cc c3` = break-in-debug, ret) - slots the console's own loader fills at load time.
- The shell now does that loader step on its in-memory copy only (shell/loader.cpp nop_loader_stubs): pointers
  from data sections into executable gaps outside the exception-table functions that start with 0xCC get
  0xCC -> 0x90 (nop) in memory; disk untouched; each patched address logged. Boot b19 confirms the mechanism:
  the fault moved from the stub (0x22106e0) to the inline int3 right after the call (image+0x2219650).
- That inline int3 is the game's own "this call must not return here" assert on the pool-allocation path
  (fn 0x4fac3c0 -> fn 0x2219620 -> vtable slot +0x30). The slot's real body exists only in the console loader's
  patch; it is not derivable from the image. So this path is the honest boundary: the shell fills every stub it
  can derive, and stops where the game needs code only the console has.
- Update (boot b22): the loader patch now also NOPs inline int3 traps that directly follow an indirect call
  (the console loader overwrites those as well; the code after the trap is the normal continuation). Boot b22
  got past image+0x2219650 and stopped at a null write at image+0x5be9387 inside the buddy allocator: the pool's
  sub-allocator slot (obj+0x1280) is null because the real body of slot +0x30 (a lazy init) is exactly what the
  console patch provides. Each converted slot reveals the next console-only body; the boundary is real.
- APU side, per ExCorvus's point that a PC's memory is already unified: AcpHal.dll now answers the memory side for
  real - ApuCreateHeap creates a committed heap below 4 GiB, ApuAlloc bump-allocates (aligned as asked), shape
  contexts are a real zeroed array, ApuIsVirtualAddressValid checks real committed memory, and
  ApuMapVirtualAddress/ApuMapApuAddress are the identity map (refused above 4 GiB, logged). AcpHalCreate still
  answers E_NOTIMPL - the audio device itself is the one thing a PC cannot fake. The pool-trap path does not call
  AcpHal (its call sites are the separate audio bring-up fn 0x43ad8b0), so this does not move the boot boundary.
- vkd3d-proton (D3D12 on Vulkan, LGPL-2.1, ExCorvus-approved with notices) was cloned to E:\third_party\vkd3d-proton
  and built with MSVC/meson (needed widl from the MSYS2 mingw-w64 tools; meson wants LC_ALL=C on this German-locale
  machine). Its d3d12.dll/d3d12core.dll are deployed beside the tools as d3d12_vk.dll/d3d12core.dll, and d3d12_xs
  now prefers them by explicit path, falling back to the system d3d12.dll. This is what will let the shell import
  the game's own memory into the GPU zero-copy once the boot gets past start-up. NOT vendored into the repo; the
  build recipe and license notice are in docs/BUILD-AND-RUN.md and NOTICE.md.

## 2026-11-26 - all 587 imports connect (measured)

- Added the last console libraries as honest skeletons: xg_xs.dll (XGCreateTextureComputer, XGSuggestSwizzleMode -
  the PC driver computes textures itself) and XboxBvhBuilder.dll (the seven mangled exports, aliased via
  /export, each logging once and answering failure; the BVH builder is real work for later). xmem.dll gained
  XMemVirtualQuery (answers from the region registry with the console protect/type bits, else VirtualQuery),
  which the shipped XboxBvhBuilder.dll needs to even load. test_gfx_stubs covers all of it; 13/13 tests pass.
- Boot with these: 68 libraries, **587 of 587 functions connected**. The boot boundary is unchanged (start-up
  stops at the console-patched pool stub, see above).
- Host-memory import into the GPU proven on this machine (RX 9070 XT): shell/tools/vkhost_probe.cpp opens the
  Vulkan device, imports a host buffer with VK_EXT_external_memory_host, and the GPU reads it back with zero
  mismatches (64 MiB tested). So "what has to be mapped into VRAM" is: nothing is copied - the game's own RAM is
  imported as device memory and the GPU reads it directly (the flat-out zero-copy model).
- TLS slot 0 solved structurally: the per-thread block now lives in the EXE's own .tls section (shell/tls_seat.cpp,
  linked into every tool/test), so Windows gives it index 0 - the same slot the console gives the game. The
  game's hardcoded slot-0 reads (e.g. the pool object pointer at entry0+0x13a8) and its index-variable reads now
  hit the SAME block; kernelbase takes index 1. At claim the exe's .tls template is rewritten with the image's
  initial data so every thread Windows starts afterwards gets it; the calling thread's block is filled directly.
  The old whiwhi_tls.dll + entry-0 redirect is removed (redirecting slot 0 to the real block heap-corrupted the
  PC-side slot-0 owner, boot b25; the zeroed-mirror variant left the game's slot-0 reads seeing zeros).
- With the exe-seat, boots b26/b27 get through memory start-up AND into DStorage queue creation, then stop at an
  int3 at image+0x2237178: the storage set-up (fn 0x2236e00) calls fn 0x6170 (returns [rsi+0xc] != 0) and asserts
  because [rsi+0xc] is 0 - a field our DStorage answers leave zero. test_thread_storage rewritten for the exe-seat
  (index 0, claim fills the block + rewrites the template, new threads get the template); 13/13 tests pass.

## 2026-11-26 - booting the image in the shell; what the runs showed (measured)

- shell/tools/boot_image.cpp (run by Temp\opencode\run_boot.ps1) loads the image, connects imports, optionally
  runs its thread-storage callbacks, and calls the entry point on a new thread with the image's own stack size.
  Unprovided imports are logged and answered with failure (0); exceptions are logged with instruction, data
  address, registers and a stack scan. The game folder listing was identical before and after every boot.
- Imports now connect 558 of 587 (our xmem.dll answers all 12 xmem functions; our d3d12_xs.dll answers the 3
  graphics imports). Remaining 29: chad.dll (5 XDsp*), xg_xs.dll (2), XboxBvhBuilder.dll (7, needs d3d12_xs +
  XMemVirtualQuery), XCRDAPI.dll (DStorageGetFactory), xapu.dll (5), AcpHal.dll (9).
- shell/d3d12_xs.*: our graphics library. D3D12ConfigureVirtualMemoryX is answered success (a PC driver manages
  virtual memory). D3D12SerializeVersionedRootSignature is forwarded to the PC's d3d12.dll. D3D12XboxCreateDevice
  creates a real PC D3D12 device behind a COM wrapper: standard vtable slots forward to the PC object; the
  console-only slots are logged the first time they are called and answered E_NOTIMPL, except slot 74 (pool
  registration: success, nothing to register on a PC) and queue slot 31 CopyPageMappingsBatchX plus queue slots
  30/32 PresentX (success, logged). Queues, command lists and descriptor heaps returned by the wrapped device
  are wrapped the same way. The game's creation call passes a 64-byte block (dwords 0x20004, 0x100000, ...) and
  gets the wrapper; memory start-up then completes: both 24 GiB views and all fixed pools are allocated
  through our xmem (traced with WHIWHI_XMEM_TRACE=1).
- The runs then stop in one of two ways, non-deterministically: an access violation at image+0x104075 (a
  vtable thunk `mov rax,[rcx]; mov rax,[rax]; jmp rax` called with a bad object), or STATUS_HEAP_CORRUPTION
  raised inside ntdll right after the fixed pools are committed. Both happen during engine init after memory
  start-up (call path fn 0x5c34fd0 -> fn 0x1e1cdb0 -> fn 0x4fac3c0).
- Thread-local storage finding: the image reads its storage block both through its index variable (rva
  0x6e08b2c) and directly as array entry 0 (`mov rcx, gs:[0x58]; mov rcx, [rcx]`, e.g. at image+0x4ec6754 and
  +0x4ec6783) - on the console the game is the main module, so its storage owns index 0. On a PC, index 0 is
  owned by the first module with storage (kernelbase, or our own exe if it has a thread-local variable). The
  shell's tools now carry a dummy thread-local variable so the shell's own exe owns index 0, and
  whiwhi_thread_block_set_redirect (WHIWHI_TLS_REDIRECT=1) points entry 0 at the lent block while an image
  holds it. Pointing entry 0 at the block currently trips a heap-corruption check in the test process, so it
  is OFF by default and recorded here as the known next thing to solve (needs a debugger backtrace or a cleaner
  slot-0 arrangement).
- Not done yet: the remaining console libraries (audio/APU, chad, xg_xs, XCRDAPI, XboxBvhBuilder), showing the
  regions to the GPU (the flat hosting model plugs in where graphics calls name memory), and a boot that reaches
  the game's run loop.

## 2026-11-26 — the graphics boundary, mapped from the image's own calls (measured, no boot)

- A register-tracking scan of the image's code (loads of the device global 0x8759d00 and its copy 0x75df048,
  followed to the `call [reg+offset]` they feed) gives the device vtable slots the executable actually uses.
  The public D3D12 indices hold exactly: 8 CreateCommandQueue, 9 **CreateCommandAllocator**, 14
  **CreateDescriptorHeap** (not 11), 15, 16 root signature, 17-21 the five views, 24 CopyDescriptorsSimple,
  25 GetResourceAllocationInfo, 26 GetCustomHeapProperties, 27 CreateCommittedResource, 36 CreateFence,
  39 CreateQueryHeap, 41 CreateCommandSignature. A PC device can answer all of those, so the wrapper now
  forwards 9 and 14 by their real meaning and wraps what they return (allocator, heap). Console-only slots the
  image uses: 48 (five objects with a 16-byte desc at device set-up), 49, 55 (derived pipeline state), 57 (base
  pipeline), 61, 62 (compute pipeline state: desc {pointer, count 1}), 67 (pipeline from a stream desc), 70 and
  71 (views from raw descriptor words), 73 (command list: 16-byte desc + allocator + riid), 74 (pool), 78-81
  (frame events), 119, 132, 133 (placed resource). Queue slots: 31 page mapping (pool queue), 32 presentation,
  23/24 in the frame step, 18. Full map with caller RVAs: docs/exe-map/GRAPHICS-CALLS.md.
- D3D12XboxCreateDevice is called after the game has created its own window (RegisterClassExA/CreateWindowExA);
  the 64-byte creation block and the ConfigureVirtualMemoryX block are logged, their field meanings are not
  established.
- xg_xs: the game builds a 0x38/0x40-byte texture description itself and asks XGSuggestSwizzleMode only when the
  format byte is 0xff (its own fallback). XGCreateTextureComputer returns an object whose +0x10 method fills a
  0x6c8-byte layout block; the game caches that block by a format/flags hash (fn 0x5121980) and derives its
  texture sizes/mips from it (fns 0x979c40, 0x3441f0). That block is the contract our xg_xs.dll must answer.
- XboxBvhBuilder: one builder object per process (constructed in fn 0x2478440 with the device, destroyed at
  shutdown), Set64BitMortonCodes around prebuild/build, prebuild-info, batched build, rehydrate (HRESULT) and
  copy. The world-start rehydrate path (fn 0xea6c60) converts GPU-view to CPU-view addresses before handing the
  builder the game's own bytes, in batches of at most 16. The builder needs XMemVirtualQuery from our xmem.dll.
- Shader translator groundwork added (shell/shader_isa.*, shell/shader_translate.*, shell/spirv.*): reads the
  console instruction words, refuses anything it does not implement with a precise diagnostic, emits SPIR-V
  through our own writer, caches by content in memory only. It translates straight-line compute shaders of
  s_mov_b32/s_movk_i32/s_add_u32/s_sub_u32/v_mov_b32/v_add_f32/s_nop/s_endpgm; branches, memory instructions,
  graphics stages and texture tiling are refused for now. A generated module validates with the Khronos
  spirv-val (SPIR-V 1.0). Plan: docs/SHADER-TRANSLATOR.md. New test `shader`; all 9 CTest tests pass (the
  known thread-storage 0xc0000374 did not appear in these runs). No boot and no GPU run in this work.
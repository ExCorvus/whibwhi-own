# Findings

What has been measured about the game and about the shell. The **Current state** block is always the latest;
below it is the dated log, oldest entry first. A later entry overrides an earlier one. Addresses written as
`fn 0x...` or `image+0x...` are offsets inside the game's executable.

## Current state (2026-11-26)

- **Build:** `shell\build.cmd` builds the shell, the console libraries and the tools; 17 tests pass.
- **Loading:** the unmodified game file is placed in memory at its preferred address. 586 of its 587 imported
  functions are connected; the one left is a ray-tracing builder function. It gets thread-local storage at
  index 0.
- **Running:** the game's entry point is started. The furthest run (boot b79) goes through the game's memory
  start-up, loads the game runtime library, opens all of the game's package files (read-only, through the
  package drive the shell answers), creates a real graphics device (Direct3D 12 on Vulkan, through vkd3d-proton,
  on the RX 9070 XT) and completes the game's graphics start-up. It then reads its whole shader package through
  the storage library (4,199 compressed pieces, each decoded by the shell's own zlib decoder into the game's
  memory), creates its textures and render targets, hands command lists to its threads and records commands on
  them (descriptor heaps, root signatures, descriptor tables, render targets, clears), submits the lists,
  signals and waits on fences, and presents: the game's display surface (3840 x 2160) is copied into a swap
  chain on the window the game itself created, one picture per interval of the game's 30-per-second frame clock.
- **What is on screen:** the game's own window (it titles it "Americas") shows a black picture. That is what the
  game's commands leave in its display surface today: it clears the surface, and every draw is held, because
  no pipeline state exists (the game's shaders are not translated yet). The shell draws nothing of its own.
- **Where it stops:** about 13 seconds into a run the game starts its audio. It asks the audio library for the
  audio device (AcpHalCreate); the shell answers "not provided", because a PC has no console audio processor;
  the game's audio start-up has no path for that failure and stops at its own breakpoint (image+0x43ad94e).
  Going further needs either a real audio path behind that object, or a decision by ExCorvus to accept a silent
  device as a stated exception to rule 4. It is not decided and nothing was built for it.
- **Textures:** a texture's bytes in the game's memory are in the shell's own untiled layout (xg_xs.h); the
  device tells the game that size when asked. A texture the game places at an address of its own memory is a
  real texture in the PC device's memory - its pixels are not at that address. Nothing copies a texture's bytes
  from the game's memory into a PC texture yet, so no texture with starting data can be sampled.
- **Network:** every imported function that could open a socket, resolve a name or make an HTTP request is
  connected to a refusal (docs/console-libs/network.md). No run so far has called one.
- **Not built:** anything that draws the game's picture. Pipeline states: the game asks for them through device
  slots 57, 62, 67 and 86 and all are refused (the shader translator handles only straight-line compute test
  shaders), so every draw is held. Command-list methods that name buffers by address (slots 41 to 48: root
  buffers, index and vertex buffers), because a game address is not turned into a PC buffer address yet. The
  command-list methods at slots 70, 71, 72, 74, 75, 126, 131, 132, 142 and 146 (what they are is not known).
  The primitive topology number 0x11 (not a PC topology). Views made from raw descriptor words (device slot 70)
  and samplers (slot 61). Reading or writing a texture the game keeps at its own address (its Map is refused).
  Device slot 81 (a per-frame query; the game tolerates the refusal). The queue's page-mapping call (slot 31)
  is recorded but nothing reads the record. The game runtime library answers every interface query with "no
  such interface". The audio device and the ray-tracing builder are skeletons that answer failure. PC command
  allocators are never reset, so memory grows while a run lasts.
- **Answers that are less than the full work** are listed in the entry "answers that say yes" below, and in the
  newest entries; each one is a place to look first when the game behaves wrongly later.
- **Nothing of the game's picture has been drawn.** The window shows the game's cleared surface.

## 2026-11-25 - the project restarts as its own shell

- All code of the earlier attempt was removed from the repository; see docs/EARLY-HISTORY.md. From here on the
  only code is our own, in shell/.
- The first two pieces: a reader for the game's executable format (shell/image.*) and an address model for
  showing the game's memory to the GPU (shell/hosting.*), each with a test.

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
- Superseded: whiwhi_tls.dll was later replaced by a block in the tools' own thread-local section
  (shell/tls_seat.cpp); see the entry on thread storage at index 0.

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

- New tool shell/tools/boot_image.cpp (run by a local runner script that is not in the repository): loads the image, connects imports,
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

## 2026-11-26 - booting the image in the shell; what the runs showed (measured)

- shell/tools/boot_image.cpp (run by a local runner script that is not in the repository) loads the image, connects imports, optionally
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
- Superseded: the dummy variable and the entry-0 redirect described here were later removed; see the entry on
  thread storage at index 0. The remaining console libraries listed here were added in later entries.

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

## 2026-11-26 - the remaining console libraries, mapped and stubbed (helper agents, reviewed and folded)

Three helpers mapped the remaining console-only libraries from the exe's own call sites (docs/console-libs/) and each shipped an honest skeleton DLL whose exports log once and
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

All 12 tests passed at that point.

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
- Superseded: the entry-0 redirect described here was later removed; see the entry on thread storage at
  index 0.

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
- At this point the shell did not change game code, so this stub was the boundary. The next entry changes
  that: the loader now replaces such stubs in its in-memory copy.
  Runtime tooling added for this: boot_image's exception handler now logs rcx/vtable on faults, continues once
  through handled breakpoints (a second occurrence at the same address ends the boot), and WHIWHI_WATCH=<rva> arms a
  1-byte write watchpoint on the entry thread (used to find the flag writer above). 12/12 tests pass.

## 2026-11-26 - loader stub patch (the shell plays the console's loader) and the precise boundary

- Census of the whole image: exactly **4** distinct int3 stub addresses are referenced from vtables
  (0x22106e0 shared by 30 slots, plus 0xb883ff, 0xb98348, 0x486c766). All are `int3` followed by the release code
  (`cc c3` = break-in-debug, ret). They are read as slots the console's own loader fills at load time; that
  is an inference (nothing in the image writes them), not something measured on a console.
- The shell now does that loader step on its in-memory copy only (shell/loader.cpp nop_loader_stubs): pointers
  from data sections into executable gaps outside the exception-table functions that start with 0xCC get
  0xCC -> 0x90 (nop) in memory; disk untouched; each patched address logged. Boot b19 confirms the mechanism:
  the fault moved from the stub (0x22106e0) to the inline int3 right after the call (image+0x2219650).
- That inline int3 is the game's own "this call must not return here" assert on the pool-allocation path
  (fn 0x4fac3c0 -> fn 0x2219620 -> vtable slot +0x30). The slot's real body exists only in the console loader's
  patch; it is not derivable from the image. So this path is the honest boundary: the shell fills every stub it
  can derive, and stops where the game needs code only the console has.
- Update (boot b22): the loader patch now also NOPs inline int3 traps that directly follow an indirect call
  (assumed to be overwritten by the console loader as well; the code after the trap is the normal
  continuation). Boot b22
  got past image+0x2219650 and stopped at a null write at image+0x5be9387 inside the buddy allocator: the pool's
  sub-allocator slot (obj+0x1280) is null because the real body of slot +0x30 (a lazy init) is exactly what the
  console patch provides. Each converted slot reveals the next console-only body; the boundary is real.
- APU side, per ExCorvus's point that a PC's memory is already unified: AcpHal.dll now answers the memory side for
  real - ApuCreateHeap creates a committed heap below 4 GiB, ApuAlloc bump-allocates (aligned as asked), shape
  contexts are a real zeroed array, ApuIsVirtualAddressValid checks real committed memory, and
  ApuMapVirtualAddress/ApuMapApuAddress are the identity map (refused above 4 GiB, logged). AcpHalCreate still
  answers E_NOTIMPL - the audio device itself is the one thing a PC cannot fake. The pool-trap path does not call
  AcpHal (its call sites are the separate audio bring-up fn 0x43ad8b0), so this does not move the boot boundary.
- vkd3d-proton (D3D12 on Vulkan, LGPL-2.1, ExCorvus-approved with notices) was cloned outside the repository
  and built with MSVC/meson (needed widl from the MSYS2 mingw-w64 tools; meson wants LC_ALL=C on this German-locale
  machine). Its d3d12.dll/d3d12core.dll are deployed beside the tools as d3d12_vk.dll/d3d12core.dll, and d3d12_xs
  now prefers them by explicit path, falling back to the system d3d12.dll. This is what will let the shell import
  the game's own memory into the GPU zero-copy once the boot gets past start-up. NOT vendored into the repo; the
  build recipe and license notice are in docs/BUILD-AND-RUN.md and NOTICE.md.

## 2026-11-26 - the last console libraries, the GPU host-memory probe, thread storage at index 0 (measured)

- Added the last console libraries as honest skeletons: xg_xs.dll (XGCreateTextureComputer, XGSuggestSwizzleMode -
  the PC driver computes textures itself) and XboxBvhBuilder.dll (the seven mangled exports, aliased via
  /export, each logging once and answering failure; the BVH builder is real work for later). xmem.dll gained
  XMemVirtualQuery (answers from the region registry with the console protect/type bits, else VirtualQuery),
  which the shipped XboxBvhBuilder.dll needs to even load. test_gfx_stubs covers all of it; 13/13 tests pass.
- Boot with these: 68 libraries, 587 of 587 functions connected in that run. (Later runs from the staged
  folder report 586 connected and one ray-tracing builder function not provided; see Current state.) The boot boundary is unchanged (start-up
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
  1 makes the game take the app-folder search; it was set on this machine with ExCorvus's approval.

## 2026-11-26 - the install folder and xgameruntime (measured)

- The game derives its install dir from GetModuleFileNameW(NULL) (fn 0x55e2910): it takes the exe's folder and
  appends the string at 0x67e2008 ("xgameruntime.dll"), then GetFileAttributesW on that. The xgameruntime load
  (fn 0x55e29dc) uses mode 2 (app-dir search, 0xa00) only when that file EXISTS next to the exe and the
  ForceUseLocalServices probe (fn 0x55e286c, HKLM GamingServices) is 1; otherwise mode 1 (System32-only, 0x800).
  On a real PC GDK install the game folder holds exe + xgameruntime.dll + the package files together.
- Superseded by the entry for boots b46-b53: the links to the game's files are not needed, because the game asks
  for its packages on drive G:\ and the shell answers that drive. What stays true is that xgameruntime.dll must
  sit beside the running program. The answer at the time was: stage a work folder that looks like that install - copy our binaries (boot_image.exe + the console
  DLLs) into it and hard-link/junction every game-folder entry into it (game folder itself untouched, listing
  hash unchanged). Then GetModuleFileNameW(NULL) is the work exe, xgameruntime.dll is present beside it (mode 2),
  and the package reads resolve through the links. run_boot.ps1 does the staging; boot_image's PEB image-path
  patch is now opt-in (WHIWHI_PEB_GAME=1) since the staged folder makes the natural path correct.
- xgameruntime.dll skeleton answers InitializeApiImpl/Ex/Ex2 = 0, QueryApiImpl = E_NOTIMPL, Uninitialize = 0,
  XErrorReport = 0; the game resolves these by name from the loaded module (fn 0x55e2a70) and calls InitEx2 with
  version constants 0x633600002712 / 0x65f400001edd.

## 2026-11-26 - the main module handle, a real backtrace, and where the latest runs stop (measured, boots b43-b45)

- The game calls GetModuleHandleA(NULL) (at image+0x1e0a377) and reads the section table of what it gets back,
  so it expects the process's main module to be itself. The shell now answers a NULL module name with the game
  image (shell/console.cpp). The latest runs stop before this call is reached, so the change has not been
  exercised by a run yet.
- boot_image now prints a real backtrace of a fault (unwinding the stack) in addition to the earlier scan of
  the stack for addresses, which had named callers that were not on the call path.
- Boot b45 stops with a read of address 0 in a system library. The backtrace is: two frames outside the game,
  then image+0x1e1cfe5, image+0x1e0a375, image+0x5c1b689. image+0x1e1cfe5 is the instruction after the game's
  call to D3D12ConfigureVirtualMemoryX in its memory start-up, so the fault is inside the shell's own
  d3d12_xs.dll while it answers that call. Earlier runs (before the staged folder) got past this call. The cause
  is not established.
- boot_image writes its entry-point log lines without taking its log lock: with the lock, the line was lost and
  a run looked as if it had ended before the entry point.

## 2026-11-26 - runtime version, the package drive, the device, and the next stop (measured, boots b46-b51)

- **The null read of boots b43-b45 was the C++ runtime version.** The game folder ships the Microsoft C++ runtime
  at version 14.29; the shell is built with compiler 14.52. The staged folder linked the game's copies beside the
  shell's tools, so the shell's own libraries loaded a runtime older than they were built for and a lock in the
  graphics library read address 0. The staged folder no longer links the game's runtime files (msvcp140*,
  vcruntime140*, concrt140, vccorlib140, vcomp140); the PC's newer runtime serves the game and the shell. Boot b46
  got past the call.
- **The game runtime library takes three arguments for an interface query**: two identifiers, then the output
  variable (rcx and rdx point at identifiers in the game's read-only data, r8 at the output; 161 call sites go
  through fn 0x55de048). The shell's function took two and wrote through the second identifier. Fixed; it now
  answers "no such interface" and logs each distinct pair once. A run asks for six pairs before the graphics
  device; the game carries on after each refusal.
- **The earlier note that the storage check at image+0x2237178 had been passed was wrong.** That run had failed
  at an earlier step and never reached the check.
- **What that check tests:** fn 0x2236e00 mounts the game's packages and then requires that at least one was
  mounted. It first asks whether the file `appdata0.rpf` exists, and it asks for it as `G:\appdata0.rpf`
  (measured with the new trace of the game's file calls, WHIWHI_FILE_TRACE=1). The console mounts the game's
  package as drive G:. This PC has no such drive, so nothing was mounted.
- **The package drive.** The shell now answers the game's file calls for `G:\` from the folder with the game's
  files (shell/console.cpp; the storage library maps the same way). It is read-only by construction: a call that
  would create, change or delete anything under `G:\` is refused inside the shell with "access denied" and never
  reaches Windows. Tested with a made-up folder (shell/tests/test_console.cpp). In boot b50 the game opened
  appdata0, common, movies, shaders, anim, data, levels, navmeshes, packs, textures, audio and script, all
  through `G:\`. The game folder listing was identical before and after every run.
- **The graphics device is created.** D3D12XboxCreateDevice is asked for the public interface identifier of
  ID3D12Device11; the shell creates a real device through the vkd3d-proton build (Direct3D 12 on Vulkan) and
  returns its wrapper. vkd3d's on-disk shader cache is turned off by the shell, so nothing is written to disk.
- **Methods that return a structure use a different argument order.** At its three calls of device slot 26
  (heap properties for heap types 1, 2 and 3) the game passes the place for the structure first and the device
  second; a PC object takes the object first. The wrapper treated the first argument as the object and faulted
  (boot b50). It now forwards these methods with the two swapped (device slots 25, 26, 43; descriptor heap 8, 9,
  10; queue 18). Only slot 26 is measured; the others are given the same order by inference.
- **Where boot b51 stops.** After the main queue (device slot 8, description {0, 0, 0, 1}) and a fence, the game
  calls device slot 48 four times with a 16-byte description, the public identifier of ID3D12CommandQueue and an
  output variable; the descriptions are {2, 0, 1, 0}, {2, 0, 2, 0}, {2, 0, 3, 0} and {2, 0, 0, 1}. On the PC's
  device, slot 48 is a different function, so this is a console-only way of creating queues. The shell answers
  "not implemented" and the game stops at its own check (image+0x2478826). What the third number means is not
  known.
- **No work folder is needed any more.** With the package drive answered by the shell, a run from the build
  folder itself (only the shell's own programs and libraries beside boot_image.exe, no game files, no links)
  reaches the same point (boots b52, b53). The run wrote nothing into that folder. docs/BUILD-AND-RUN.md
  describes this way of running; the earlier staged folder with links to the game's files is superseded.

## 2026-11-26 - the game's graphics start-up completes; the next stop is the texture layout library (measured, boots b54-b64)

The game folder listing was identical before and after every run. docs/exe-map/GRAPHICS-CALLS.md section 2.2b
has the table of every device call seen, with addresses.

- **The game always gets the shell's own objects.** vkd3d-proton checks that the objects it is handed are its
  own, so the method table of a real PC object cannot be replaced. Every object the device returns is wrapped,
  and a wrapper the game passes back as an argument is replaced by its PC object before the PC library sees it.
- **A wrapper's reference count started at 0 instead of 1** (an initialisation mistake in the shell). Fixed.
- **Queues, lists, frame clock.** Device slot 48 creates four more queues and slot 73 the command lists; both
  take a 16-byte description whose first number is the public type; the remaining numbers are not understood
  and not applied. Device slots 78, 79 and 80 (frame interval, frame events, waiting for an event) are answered
  by a clock in the shell at the interval the game sets (33,333 microseconds in the runs). The clock is a
  timer, not a display. Four choices in it are the shell's own and not measured console behaviour; they are
  written in the comment above the clock in shell/d3d12_xs.cpp.
- **A buffer over the game's own memory.** The game reserves one range, splits it into five pieces, maps pages
  into them (one run of pages crosses from one piece into the next; the memory library now allows that) and
  asks the device (slot 133) for a buffer of 182,452,224 bytes at an address inside it. The shell opens that
  memory as a heap on the PC device and places a real buffer on it: the GPU device has the game's memory, with
  no copy. The same is done for the game's 6,236 MiB page pool when the game registers it (slot 74).
- **The game's allocation routines.** The game exports a table named `__XMemAllocationHooks__` with an allocate
  and a free routine. The shell's memory library now sends XMemAlloc and XMemFree through them (inferred from
  the name and from the routines' own structure; the console's memory library was not read). The game uses
  this to choose where a descriptor heap's memory lies and then checks it; with the routines connected that
  check passes. Descriptor handles the game passes are moved from that address range to the PC heap's handles.
  Not established: whether the game also writes descriptor records into that memory itself.
- **Structure-returning methods.** Measured in runs: a 16-byte result is asked with the place first and the
  object second (device slots 25, 26); an 8-byte result with the object first, answered in the register
  (descriptor heap slot 9).
- **Display surfaces.** Device slot 127 has the argument list of the public CreateCommittedResource3; the game
  creates two 3840 x 2160 surfaces with the "can be displayed" heap flag, and the shell creates real PC
  surfaces. The game also expects the graphics library to take those surfaces' memory from a 64 MiB block of
  its own through XMemAlloc, and stops itself unless nearly all of that block was taken. The shell's library
  makes that request for each surface, as the console's library is inferred to. The pixels are in the PC
  device's memory; nothing is stored in the block. (This is in the list below.)
- **Command signatures.** The game creates five. Four are real PC signatures. One has argument type 105 with
  12-byte records, which a PC device does not have and whose meaning is not known; it is kept as a description
  only, and executing it draws nothing and logs that.
- **The command list's method order is not the public one above slot 26.** The game executes its signatures
  through list slot 61 with the arguments of ExecuteIndirect; the public list has that at slot 59. The shell
  now forwards only slots 3..26 (three of the game's calls agree with the public order there) and slot 61; all
  other list slots answer "not provided" and log their arguments.
- **Next stop.** After the graphics start-up the game calls device slots 61 and 86 (not provided, tolerated),
  then XGSuggestSwizzleMode and XGCreateTextureComputer in the texture layout library. Both are refused, and
  the game faults at image+0x16f71fb inside its texture creation (call chain +0x193df50, +0x34c001,
  +0x19f545f). What the game asks of that library is in docs/exe-map/GRAPHICS-CALLS.md section 3.

## 2026-11-26 - answers that say yes with less than the full work behind them

Rule 4 says a library function must not answer success for work that was not done. These are the places where
the shell's answer is success, or an object, with less behind it than on the console. Each is stated at the
place in the code as well. They are the first places to look when the game later behaves wrongly.

| where | what is answered | what is behind it |
|---|---|---|
| xgameruntime.dll, InitializeApiImpl / Ex / Ex2 | success | nothing is initialised; every interface query is then refused |
| xmem.dll, XMemIsToolsMemAvailable | yes | a PC has no tools memory; the meaning of the answer to the game is not known |
| xmem.dll, XMemTransferMemory | success | nothing is moved |
| xmem.dll, XMemGetWorkingSetStatistics | success | the PC's memory totals, at field positions that are guessed |
| d3d12_xs.dll, D3D12ConfigureVirtualMemoryX | success | nothing is configured |
| XCRDAPI.dll, SetStagingBufferSize | success | there is no staging buffer; reads go straight to the destination |
| d3d12_xs.dll, frame interval and frame events | success | a timer in the shell, not a display |
| d3d12_xs.dll, queues (slot 48) and lists (slot 73) | real PC objects | two or three numbers of each description are ignored |
| d3d12_xs.dll, resource descriptions (slots 25, 127, 133) | real PC objects | console-only flag bits 0x20000 and 0x300000 are dropped |
| d3d12_xs.dll, display surfaces (slot 127) | real PC surfaces | the memory block the game budgets for them is requested but not used |
| d3d12_xs.dll, command signature with argument type 105 | an object | a description only; executing it draws nothing |
| fences | real PC fences | the PC library ignores the "shared" flag the game sets |
| d3d12_xs.dll, the queue's page-mapping call (slot 31) | success | the record of which pool page each view page shows is exact, but nothing reads it yet |
| shell/loader.cpp | - | breakpoint bytes in stubs are replaced in memory (the one stated exception in rule 4) |

Outside the repository: one registry value was set on the development PC with ExCorvus's agreement
(`ForceUseLocalServices`, described in docs/BUILD-AND-RUN.md).

**Network.** Rule 3 says nothing online. When this entry was written the game's network imports were connected
to the real libraries with nothing refusing them. Superseded: the boundary is built (see the entry "the network
boundary" below).

## 2026-11-26 - the queue's page-mapping call is recorded (measured layout; not yet reached by a run)

- What the game passes, read in its page mapper (fn 0xf2c470, the call is at 0xf2cc7f): the number of batches,
  the batches, the pool address, the pool size in 64 KiB pages as a 32-bit number, and a 32-bit 0. A batch is
  24 bytes: the destination address in the GPU-only view, the number of ranges, a pointer to the ranges. A range
  is 12 bytes: a kind, a pool page number, a count. The game builds two shapes only: to show pages, one range of
  kind 1 with count 1 per page, naming the pool page; to stop showing pages, one range of kind 2 with page 0 and
  the number of pages. The ranges of a batch follow each other from the destination.
- The shell's graphics library now keeps that as a record: for every 64 KiB page of the view, the pool page it
  shows (shell/d3d12_xs.cpp). A call is checked completely before anything changes. A shape the game was not
  seen to build (another kind, a showing range with a count above 1) is refused and logged, not guessed. The pool
  named must be the one the game registered with the device.
- Why a record and not more: the console's graphics processor has page tables for this view. A PC one does not
  share the process's addresses. The pool is already open on the PC device as a heap, so a pool page is an offset
  in that heap, and the record is what turns an address in the view into such an offset.
- What is missing: nothing reads the record yet. Views made from raw descriptor words (device slot 70) and
  translated shaders will have to. One open design point is recorded here so it is not lost: a span of the view
  is contiguous for the game but its pool pages are not, so a PC buffer over such a span needs either a sparse
  resource whose pieces are bound to pool pages, or the same pages mapped a second time in the process and
  opened as a heap. Neither is built or tested.
- Tested without a device (shell/tests/test_d3d12_xs.cpp) with made-up addresses: showing, showing again,
  removing, two batches in one call, and every refusal leaving the record unchanged. No run has reached this
  call: the latest run stops earlier, at the texture layout library.

## 2026-11-26 - the network boundary, the first texture description, and what the command list really is (boots b65-b66)

**The network boundary is built** (shell/offline.*, connected in shell/console.cpp; details and sources in
docs/console-libs/network.md).

- Measured: the game imports 78 network functions - 25 from the Windows socket library (23 by ordinal), 15 from
  the Windows HTTP library, 38 from the HTTP client shipped with the game.
- 49 of them could open a socket, resolve a name or build an HTTP request. Each has a replacement with the
  public argument list that touches no network, sets the documented error, returns the documented failure
  value, logs once and counts its calls. The other 29 only read, release or convert something and go to the
  real libraries.
- One decision taken when connecting it, which ExCorvus can reverse: starting the socket library (WSAStartup) goes
  to the real library. It initialises the library inside the process and sends nothing; the game stops in its
  own check if it fails (measured: one call site, a breakpoint on any failure). Creating a socket is refused, so
  nothing can go out either way. The stricter refusal stays in the code, tested, but is not connected.
- Not known: whether a run reaches the game's network start-up at all. Boot b66 ended without a single network
  call. At the end of a run the tool prints how often each function was refused.

**The texture layout library: the argument lists were wrong, and the first description is measured**
(docs/console-libs/xg-xs.md).

- Measured at all four call sites: XGCreateTextureComputer takes two arguments, the description and a place
  for the object. Ours declared four and never cleared the place, so the game used a leftover value. Fixed: both
  functions take what the game passes, still refuse, clear the place, and log each distinct description.
- Measured in boot b65: the first texture is two-dimensional, 256 x 128, one mip, format code 61, element size
  1 byte, flags 0x300000. 61 is the public DXGI number for an 8-bit single-channel format, which is 1 byte per
  pixel; the block-format code ranges the game tests (70-84, 94-99) are the public DXGI numbers of the
  block-compressed formats. Inferred from these: the format codes are DXGI format numbers, so the size of every
  mip can be computed from the public format list. Two agreeing cases, not a proof for every code.
- The call site that does not check the answer (fn 0x16f7100) then calls the object's method at +0x40 once per
  mip of each slice with (object, destination base, 0, subresource number, source pointer, bytes per row,
  rows), and the method at +0x08 at the end. Measured: the destination base is the same for every call, so the
  object itself adds each subresource's position.
- The game reads seven fields of the 0x6c8-byte layout block (a 0x38-byte header and 15 records of 0x70 bytes).
  It compares two of them with its own tightly packed sizes and copies directly when they are equal. So a layout
  that is simply the game's own packed order is one the game accepts without a second buffer. Not built yet.

**The command list is an object the game writes into** (docs/exe-map/COMMAND-LIST-ORDER.md, with review notes).

- Measured: the game keeps a write position (list +0x18) and an end (list +0x30) inside the list object and
  writes drawing records there itself; it calls a method only when the position has reached the end. It also
  writes two 16-byte blocks at list +0x544 and +0x554 without any check.
- Consequence for the shell, not yet fixed: our list object is 0x50 bytes, so those two writes land outside it.
  No run has reached a draw, so this has not happened yet; the object must be made large enough before one does.
- Inferred from the argument shapes of 15 slots: the methods the game falls back to are the ordinary public
  command-list methods (console slots 20-26 are public 20-26; console 33-50 are public 29-46). Four console-only
  methods sit at slots 27-32 and two public ones are missing before slot 61. To be confirmed by a run.
- Design choice that follows: keep the position equal to the end, so every drawing command arrives as a method
  call that can be answered with the same call on the PC list. The records the game would otherwise write are
  hardware command words; decoding those would mean recreating the console's graphics hardware, which this
  project does not do.

**Three device methods identified by the same reading** (measured argument lists; the public names are inferred):
slot 61 creates a sampler at a position in the game's descriptor memory from a console-layout description;
slot 70 writes a view at such a position from two description blocks; slot 86 is the public pipeline-state
creation from a stream (its identifier is the public pipeline-state identifier). None is answered yet.

## 2026-11-26 - the game reads its shaders, makes its textures and starts recording commands (boots b67-b74)

Each run left the game folder unchanged.

- **Objects the game is given.** Every graphics object now starts with 4 KiB the game may read and write (the
  console's objects have fields the game uses directly), and the shell's own fields come after. Two values in
  that area are the shell's choice, not measured console values: the device's byte at +0x18 is 1, which makes
  the game send descriptor copies through the device instead of copying 32-byte records itself; and a command
  list's write position (+0x18) equals its end (+0x30), which makes the game call a method for every command
  instead of writing hardware command words into the list. At the end of a run the graphics library reports
  what the game wrote into those areas; so far nothing.
- **Texture layout library (xg_xs.dll).** The description both functions take is the game's resource
  description: the same block, from the same builder, is what the game hands the device when it asks how much
  memory a texture needs, and it has the public resource description's fields in the public order (the field
  at +0x24 is samples per pixel, +0x2c a console tiling number - the game itself writes 0x105 there for a
  volume). The library answers with an untiled layout of its own: rows packed one after another, mips one
  after another, array slices after that; formats from the public format list only. The object's methods at
  +0x40 and +0x48 copy rows into and out of texture memory (arguments measured: object, memory, 0,
  subresource, rows, bytes per row, bytes per two-dimensional slice; +0x48 has the buffer second and the
  memory sixth). XGSuggestSwizzleMode is refused; the game carries on.
- **How the game makes a texture (measured, fn 0x193de10).** With starting data and no memory given: it asks
  the device for the size and alignment (slot 25, or slot 108 = the same question with a longer description
  and a 24-byte record), takes that much of its own memory (fn 0x2474690), has the layout object copy the
  rows in, and creates NO resource object: the texture is memory plus a description, named later by address
  in a view (device slot 70). A surface that can be displayed: a committed resource (slot 127). Anything else
  (render targets, depth, textures without data): a resource placed at an address of its own memory (slot
  133) whose fourth argument is the public initial-layout number (6 and 3 were seen).
- **Size and alignment (slots 25 and 108).** The game asks with alignment 0x1000 when its own estimate is at
  most 0x80007 bytes and asks again without it unless the answer's alignment is 0x1000; it stops itself when
  the size needs more than 32 bits. With the console tiling number 0x105 in the description the PC device
  answered failure, which stopped the game (boot b68, image+0x344196). The shell now answers for textures
  itself: the total of its own untiled layout, rounded up to 0x1000 when asked, to 64 KiB otherwise. Buffers
  still go to the PC device.
- **Textures placed at an address (slot 133).** Answered with a real PC texture of the same description, kept
  as standing at that address. Seen: 1 x 1 render targets and depth targets, 2560 x 1440 format 10 with flags
  0x300005, 1280 x 720 format 42 with flags 0x300004 and tiling number 0x102. Console-only flag bits seen and
  dropped: 0x300000, 0xc00000, 0x4800000. **The pixels are in the PC device's memory, not at the game's
  address**; Map on such a texture is refused.
- **Buffers placed at any address (slot 133).** The game placed a 16-byte buffer at an address ending in
  0x6fc0; the PC device places resources only on 64 KiB boundaries and refused (boot b72). The PC buffer now
  starts at the boundary below and the distance is added wherever the game names an offset inside the buffer
  (buffer copies, indirect execution). Map on such a buffer answers the game's own address.
- **Storage library.** The request the game fills is eight 64-bit words: options (0 = copy file bytes, 1 =
  the bytes are one zlib stream), destination, zero, destination bytes, file object, file offset, source bytes,
  zero. Read off a run and confirmed by behaviour: all 4,199 pieces of the shader package decoded to exactly
  65,536 bytes each. The decoder is the shell's own (shell/inflate.*, from the public format description).
  The queue's method at +0x20 is given the status array and an entry number (the public EnqueueStatus); a
  request that is not carried out now answers a failure and the next status entry carries it. The queue's
  method at +0x30 is called with (a pointer, 0x7ffffffffffffffc, 0) from image+0xe894a; what it is has not
  been read, it answers "not provided" and the game carries on.
- **Structure-returning methods, corrected.** Two orders are measured: the device takes the place for the
  structure first and itself second (slots 25, 26, 108); a resource takes itself first and the place second
  (slot 10, its 56-byte description, image+0x501f80b). A descriptor heap's 8-byte start is asked with the
  heap alone and read from the register. An earlier version answered every object-first call in the register,
  which left the resource's description unwritten.
- **The queue's method order is not the public one.** Measured: slot 18 is called with the address of one
  64-bit number and its result is checked as a success code (the public timestamp frequency, public slot 16);
  slot 16 is called with a fence and a value (the public Signal, public slot 14); slot 35 is called with a
  count, an array of lists, the frame token and a 32-bit 0 (submission; image+0x2482689). Until boot b73 the
  shell forwarded queue slots 8 to 17 by the public order, so the game's Signal call would have reached the PC
  queue's frequency method with a fence where a number's address belongs. No queue slot is forwarded by the
  public order any more.
- **Command allocator slot 9** (console-only): the game names one of its functions (image+0x555df10) and its
  rendering context. That function cuts memory out of the context's frame memory: on the console the
  allocator gets its command-buffer memory from the game. The shell keeps it on record and never calls it.
- **Command list slot 139** (console-only): (list, allocator, 0, 0), a negative result stops the game. It is
  how a list starts recording; answered with the PC list's Reset. The game never resets its allocators, so
  the PC allocators grow for as long as it runs - not handled yet.
- **Command list slot 32** was called with a count of 2 and an array of two descriptor heaps: the public
  SetDescriptorHeaps, which the public list has at slot 28. Together with the earlier measurements this puts
  console slots 32 to 52 four above the public ones (33/34 root signatures, 35/36 descriptor tables, 47/48
  index and vertex buffers, 50 render targets, 52 clearing a render target - slot 52 was called with a
  descriptor address, a pointer to four numbers, 0 and 0). Not installed yet.
- **Fence slot 12** (console-only): called with the fence alone; the game reads a 64-bit number THROUGH the
  result, compares it with -1 and with the value it waits for. So it returns the address of the fence's
  completed value. This is where boot b74 stops.
- **Pipeline states.** After reading the shaders the game asks for pipeline states through device slots 67,
  62 and 57 (and 86 at start-up), each with the public pipeline-state identifier. All refused; the game
  carries on without them.

## 2026-11-26 - frames are recorded, submitted and presented to the game's window; the stop is audio (boots b75-b79)

Measured in the game's code and in runs. The game folder listing was identical before and after every run.

**Fences.** The game asks a fence for the *address* of its completed value (fence method slot 12) once, keeps the
address, and then reads the number at it again and again without calling anything, sleeping between looks,
until the number reaches the value it wants; all ones means the device is lost (fn 0x4fb0ad0). So the console
exposes a fence's completed value as a live number. The shell keeps a 64-bit number in each fence object,
answers its address, and brings it up to date from the PC fence whenever the game signals the fence through the
queue (a wait on the PC fence's event) and at every call. Fence slot 8 is the public GetCompletedValue
(called with the fence alone, result compared with all ones; +0x4dad098).

**The queue.** No queue method is passed on by the public order. Measured and written out: slot 16 = Signal
(fence, value); slot 18 = GetTimestampFrequency; slot 35 = the console's submission (count, array of lists, the
frame token, a last number). The last number is 0 where the game submits its frame lists (+0x2482689) and 3
where its present function submits the lists its threads recorded, up to 256 at a time (+0x24821dd); what the
number tells the console is not known, both are carried out as the PC queue's ExecuteCommandLists and the
number is only logged. The game never closes a list after recording (the PC queue only takes closed lists and
treats a recording one as a fatal error of the whole device), so a list that is still recording is closed on
the PC at submission.

**Command lists.** The inference that console list slots 32 to 52 are the public methods four lower held for
every call a run made and the shell could check: 32 SetDescriptorHeaps (two heaps of ours), 34
SetGraphicsRootSignature, 36 SetGraphicsRootDescriptorTable (handles inside a shader-visible heap), 50
OMSetRenderTargets (views the game made through the device), 52 ClearRenderTargetView. Each of these is
answered by the same call on the PC list after its arguments are checked; a call whose arguments do not have
the expected shape is logged and not passed on. Draws are passed on only when a pipeline state is set, which
never happens yet. The game also sets primitive topology number 0x11 (list slot 20), which the PC library does
not have; it is not passed on and draws are held while it is set.

**Presentation.** The game's present call is queue slot 32 (+0x24824c1): (plane count, plane records, present
parameters). A plane record is 0x48 bytes: the frame token, a count of 1, a pointer to the surface pointer, a
pointer to a rectangle {0, 0, width, height}, a pointer to 16 more bytes, a colour-space number. The surface is
one of the two 3840 x 2160 display surfaces the game created. The shell copies it into the next buffer of a
swap chain on the game's own window and presents that buffer; the swap chain is the PC library's own and scales
to the window. The game's window has the class name sgaWindowClass and the title "Americas"; the game creates
it hidden, and the shell shows it, because the window is where the picture goes. The game runs its own
message loop in its end-of-frame function (fn 0x24846d0). A picture of the window taken during boot b79 shows
a black client area: the game's cleared surface.

**Pacing.** Where it presents, the game does not pace itself: at a frame's start it only asks the frame clock
for the current frame without waiting (+0x2482448), and before a present it waits for event type 2 of the frame
two back (+0x2482405), which has long passed on the shell's clock. Left alone it presented about 500 times a
second (boot b76). A display shows one picture per interval, so the shell's present takes the next interval of
the frame clock that has not shown a picture yet and waits for it (at most two intervals). With that, boots
b77 to b79 present about once per interval. This is the shell's choice; what the console does is not known.

**Other calls seen.** Device slot 81 is called with (a frame token, 0x11100, a place holding the number 6, a
place for records) and its failure is tolerated (+0x2482710); it is not answered. Fence slot 8 as above.

**Where the runs end.** About 13 seconds in, on another thread, the game starts its audio: ApuCreateHeap
(0x1c700000 bytes, answered with real memory below 4 GiB), AcpHalAllocateShapeContexts (350), ApuAlloc, then
AcpHalCreate, which the shell refuses. The game stops at its own breakpoint at image+0x43ad94e. The game
drives the audio device object through four method offsets and waits on a status number only the console's
driver writes (docs/console-libs/acp-xapu.md), so there is no small honest answer: it is either a real audio
path or a silent device accepted as a stated exception. That decision is ExCorvus's.

**Answers that are less than the full work (added by this entry).** One picture per frame-clock interval is the
shell's own pacing. The submission's last number and the frame token are not used. The present's rectangle,
colour space and parameters are not used, and only the first plane is shown. The surface is copied without
tracking the state the game left it in. The window is shown by the shell. Lists are closed by the shell at
submission. PC command allocators are never reset.

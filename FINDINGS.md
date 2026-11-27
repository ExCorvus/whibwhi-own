# Findings

What has been measured about the game and about the shell. The **Current state** block is always the latest;
below it is the dated log, oldest entry first. A later entry overrides an earlier one. Addresses written as
`fn 0x...` or `image+0x...` are offsets inside the game's executable.

## Current state (2026-11-26)

- **Newest (game test 23:13):** run with the installation's own libraries (the ray-tracing builder, FidelityFX,
  Bink, libHttpClient load from the copy). The run stops about 5 s in, inside the installation's ray-tracing
  builder: its shaders call 33 console shader functions named `xbox.` (AMD GPU operations, see the entry "the
  builder's shaders call the console's own shader functions"), which vkd3d-proton's shader converter does not
  have. vkd3d-proton is built from the fork ExCorvus/vkd3d-proton (branch whibwhi-own) with ExCorvus/dxil-spirv
  (branch whibwhi-own: reads `insertvalue`). The shell changes of that run are not committed yet: no game test has
  passed on them. The blocks below are older.
- **Returned to this state on 2026-11-26** (bugfix: undid an AI trap). Everything built after boot b80 was
  taken out of the repository again; see the last dated entry. This block describes the b80 state, re-measured
  in boot b169.
- **Build:** `shell\build.cmd` builds the shell, the console libraries and the tools; 27 tests pass (the shader translator came back, see the newest entries).
- **Testing (rule 11):** the shell is tested on the game itself: scripts/game_test.ps1 runs the real game from the
  user's copy, counts every import call and compares with the last accepted run. The sample tests are a build
  check only. Why this came so late is in the entry "what went wrong: the shell was not tested on the game's own
  code".
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
  device as a stated exception to rule 4. It is not decided and nothing was built for it. This is the only
  stop left at that point (boot b80); the game has no switch of its own for running without audio hardware.
- **Textures:** a texture's bytes in the game's memory are in the shell's own untiled layout (xg_xs.h); the
  device tells the game that size when asked. A texture the game places at an address of its own memory is a
  real texture in the PC device's memory - its pixels are not at that address. Nothing copies a texture's bytes
  from the game's memory into a PC texture yet, so no texture with starting data can be sampled.
- **Network:** every imported function that could open a socket, resolve a name or make an HTTP request is
  connected to a refusal (docs/console-libs/network.md). No run so far has called one.
- **Pipelines (boots b172-b174):** device slot 86 writes the game's vertex and pixel programs as PC shaders bound
  to the shell's layout and makes real PC pipelines with the shell's one root signature; the PC device took every
  one written (32,768+ in b174). The game's start-up waits for about 50,000 of them, which takes longer than
  300 s at the PC device's compile rate. Slots 57, 62 and 67 are not provided.
- **Not built:** anything that draws the game's picture. Draws are held while a pipeline of the shell's layout
  is named, because the arguments buffer and the shell's heaps are not filled from the game's root arguments
  yet. Command-list methods that name buffers by address (slots 41 to 48: root
  buffers, index and vertex buffers), because a game address is not turned into a PC buffer address yet. The
  command-list methods at slots 70, 71, 72, 74, 75, 126, 131, 132, 142 and 146 (what they are is not known).
  The primitive topology number 0x11 (not a PC topology). Views made from raw descriptor words (device slot 70)
  and samplers (slot 61). Reading or writing a texture the game keeps at its own address (its Map is refused).
  Device slot 81 (a per-frame query; the game tolerates the refusal). The queue's page-mapping call (slot 31)
  is recorded but nothing reads the record. The game runtime library answers every interface query with "no
  such interface", except the system interface, where one method is answered: the sandbox name. The audio
  device and the ray-tracing builder are skeletons that answer failure. PC command
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

## 2026-11-26 - the sandbox name is answered; audio is the one stop left (boot b80)

**The second stop of boots b75 to b79, read.** Besides the audio stop, those runs hit a breakpoint at
image+0x3dda76b. The function there (fn 0x3dda560, a method reached through an object's table from fn 0x3da1560)
asks the game runtime library for two things and then checks one of them:

- pair `{973a344e-24bf-4d0f-8457-56c534892b29}` / `{2549f142-6419-4a06-97b5-931aab7c2f34}`, method at +0x18,
  one argument (a place for a 32-bit number). The result is not checked; the number is formatted into a text.
  Not answered. (By its shape it could be the public XGameGetXboxTitleId; that is a guess.)
- pair `{e349bd1a-fc20-4e40-b99c-4178cc6b409f}` / `{1861cf2e-e18b-4834-a9f5-b4a4e6efb4cf}`, method at +0x20,
  called with (32, a text buffer, a place for a size). The game then compares the text with names it holds in
  scrambled form in its code: `RETAIL`, `CERT`, `RKTR.99`, `CERT.DEBUG`, or anything beginning with `RKTR`, and
  stops at the breakpoint when none matches. With the query refused the text stayed empty, hence the stop.

That argument list and those values are the public `XSystemGetXboxLiveSandboxId(sandboxIdSize, sandboxId,
sandboxIdUsed)`. Microsoft's public page for it says the sandbox ID is stored on the device and "defaults to
'RETAIL'" when none is found.

**What the shell answers.** Our runtime library now provides that one interface with that one method: it
answers the sandbox name this PC stores, or `RETAIL` when it stores none (this PC stores none). Where a PC
stores it is taken to be the registry value `Sandbox` under `HKLM\SOFTWARE\Microsoft\XboxLive` (the place
Microsoft's own sandbox switching script for PCs writes; taken from memory, not re-verified). The two other
methods the game calls on that interface (+0x18 and +0x28, three arguments each, an error code back) and every
other position are logged and answered "not implemented". Boot b80: the name was asked for, answered `RETAIL`,
and the game went on (it reads more files afterwards). The shell still refuses every network call, so the name
does not put anything online.

**Related, measured on the way.** The game's device-type check (fn 0x55deb88, used by the memory start-up)
does not ask the runtime library: it reads the registry value `ConsoleMode` under
`HKLM\Software\Microsoft\Windows NT\CurrentVersion\OEM` and answers "PC" (1) when the key is absent, which
is the case here. The memory start-up accepts PC and the three Series device types.

**Audio is the one stop left, and the game has no switch for it.** The audio set-up (fn 0x43a6420) calls the
hardware start-up only when the byte at image+0xaa31c54 is 1. That byte has one writer in the whole image
(fn 0x25a4c00), which stores the constant 1. So this build always starts audio hardware; there is nothing of
the game's own to turn it off with, and the shell does not change that byte. The choices are unchanged and are
ExCorvus's: a real audio path behind the audio device object, or a silent device accepted as a stated exception.

## 2026-11-26 - bugfix: undid an AI trap; the repository is back at the state of boot b80 (re-measured, boot b169)

ExCorvus's verdict: the game and the shell are one piece of software, and the work after boot b80 started to
treat them as two. After b80 the agents rewrote the game's shaders into separate PC shaders, then built PC root
signatures, PC pipelines, PC copies of textures, a PC audio path and further layers around those, each one
answering the game with a second program of the shell's own design instead of being the system the game expects.
Every such layer made the next one necessary. All of it is taken out: every file under `shell/`, `scripts/` and
`docs/`, and NOTICE.md and FINDINGS.md, are restored to commit e288f85 (the last commit of the b80 state). Kept
from later: README.md, AGENTS.md (rule 7 as changed by ExCorvus), ATTRIBUTION.md and docs/EARLY-HISTORY.md. The
later work is not lost; it is kept outside the repository by ExCorvus's choice.

Re-measured from e288f85, built on its own (boot b169, 40 s asked, game folder unchanged): the game is placed and
started, reads its files, creates the graphics device, records and submits command lists and presents its
display surface to its window (16 or more frames, black). Its pipeline requests (device slots 57 and 86) are
answered "not provided", and its start-up goes on at once. About 13 seconds in, it asks for the audio device
(AcpHalCreate), which is not provided, and stops at its own breakpoint at image+0x43ad94e, unwind +0x43a657a
+0x25a4e47 +0x25a4c98 +0x1d857a6.

Measured with the later work, for comparison (boots b163-b166): when pipeline requests are answered with real
PC pipelines, the game's start-up waits for them (about 35,000 in 200 s, one frame presented), so the shaders are
tied into the game's own frame timing.

**The idea kept from the later work (ExCorvus): compare the game's Direct3D 12 with the normal one.** The game
uses the console's variant of Direct3D 12. Each call it makes is to be set side by side with the normal (PC)
Direct3D 12: where the two are the same, the shell is the normal one; where they differ, the difference is read
from the executable (how the game calls it, what it reads back, when it waits) and the shell is what the game
expects there. A first case to read: before it asks for a pipeline, the game checks a pipeline store of its own
(fn 0x2479f40 on the store at image+0x75ddb90, called from fn 0x2479760 before device slot 86).

## 2026-11-26 - the game's pipelines handed to the normal call unchanged: all refused (measured, boots b170-b171)

The comparison of the game's Direct3D 12 with the normal one (docs/exe-map/GRAPHICS-CALLS.md section 2.2c) shows
device slots 86 and 67 with the same three arguments as the normal CreatePipelineState: a {size, stream}
description, the pipeline identifier and the place for the result. So the shell now answers them as the normal
call: the stream is passed to the PC device's CreatePipelineState unchanged, except that a root signature of the
shell's own is replaced by the PC object behind it (`w_device_stream_pipeline` in shell/d3d12_xs.cpp).

Boot b170 (40 s, game folder unchanged): every stream walks by the public subobject layout (688 bytes, types
0 1 2 3 4 5 8 9 10 21 17 12 14 15 16: root signature, five shader stages, blend, sample mask, rasterizer, depth
stencil 1, sample description, input layout, topology, render-target formats, depth-stencil format). The PC
device refused all of them: 32,768 of 32,768 with 0x80004005 (E_FAIL), none made. The game takes the refusal
and goes on; the run ends where b169 ends, at the audio device (AcpHalCreate) and the game's breakpoint at
image+0x43ad94e. Boot b171 with vkd3d-proton's error log switched on (VKD3D_DEBUG=err, VKD3D_SHADER_DEBUG=err)
printed no reason.

So the normal call does not take the game's shaders as they are: the stream is the normal one, its shader stages
are not. That is where the game's Direct3D 12 differs from the normal one for pipelines. What the console device
does with these shader containers is to be read from the executable and the containers next.

## 2026-11-26 - the game's pipelines made from its shaders written against the shell's layout (measured, boots b172-b174)

The shader translator came back from the saved work, bound to one layout of the shell's own
(shell/shader_pc.hpp, `shell_layout`): every console program is written as a PC shader that reads its starting
registers from one arguments buffer and takes its descriptors from the shell's heaps by place. Device slot 86
now writes the stream's vertex and pixel programs that way, names the shell's one root signature instead of the
game's, and hands the rest of the stream to the PC device's CreatePipelineState unchanged
(`w_device_stream_pipeline` and `shell_layout_root_signature` in shell/d3d12_xs.cpp). Written shaders are kept in
memory and each container is written once.

The shell's root signature (19 parameters: a sampler table, the arguments buffer, one table over the resource
heap with 12 ranges, and 16 root buffers for addresses) is accepted by the PC device.

Boot b172 (60 s, game folder unchanged): the PC device made every pipeline whose shaders were written: 16,384
made by the last log line, none refused by the device. The 384 not made were refused by the translator itself
for three reasons (pixel programs only): a texel read whose values go to an instruction the pass cannot type,
`MIMG 32` with a sampler the pass cannot tell, and a resource taken from the descriptor heap by number.

Slot 67 is not the same call as slot 86: at the image's call that returns to image+0x24800db its first argument
is not a {size, stream} pair (the "size" read was 0x260a...). The earlier reading in GRAPHICS-CALLS.md section
2.2c that slots 67 and 86 have the same three arguments was a guess from the comparison and is wrong for 67. Slot
67 is back to "not provided" until its shape is read.

Boot b174 (300 s, game folder unchanged, no fault of the driver or the machine): about 44,700 pipeline requests,
32,768 made by the last power-of-two line, 714 shaders written, 89,428 taken from memory. The game presented one
frame and its start-up was still waiting for pipelines when the run ended (boot b141 of the saved work saw about
50,000 asked before the game moved on). So the start-up is bound by how fast the PC device compiles pipelines.

Draws are held: a list that names one of these pipelines does not set it on the PC list, because its draws need
the arguments buffer and the shell's heaps filled from the game's root arguments, which is not built yet. No list
named a pipeline in these runs (the start-up had not finished).

## 2026-11-26 - what went wrong: the shell was not tested on the game's own code, so the heaps went in and out

What happened, written down at ExCorvus's request as research on AI agents (2026-11-25 to 2026-11-26):

1. The shell was never tested on the game's own code. Its checks were sample tests on hand-made programs and
   containers (17, later 27) and "how far a run gets". Neither took the game's native code as the reference.

2. Without that reference, the AI agents doing the work (several models, one after another) judged their own
   work by their own samples. Work that passed them was committed as progress. Later it was found wrong against
   the game, removed, and then put back. The descriptor heaps went in and out most often:
   - a PC root signature rebuilt for every pipeline from the game's console layout (shader_root): dropped;
   - descriptor records written into the game's heap memory and translated again at every submission: built,
     crashed after 6 s (boot b167), reverted;
   - the shader translator bound to heap places of a layout of the shell's own: built again on 2026-11-26.
   The same happened around the heaps:
   - every pipeline request translated both of its shaders again (34,136 requests in boot b165), until a cache
     came after ExCorvus said "they are getting retranslated";
   - pipeline threading: a render-thread queue and a frame-wait pump, both stalled the game (boot b142), both
     reverted;
   - audio: a silent device that answered success was undone; real audio was built and then taken out again;
   - the whole repository: 69 commits took out everything after boot b80 ("bugfix: undid an AI trap"), and an
     hour later 35 commits brought the shader translator back.

3. Why (ExCorvus's reading, which the record above supports): because the shell was not tested on the game's
   native code, the agents translated the heaps (and the other shims) in and out. Each turn rebuilt what the turn
   before had removed, because nothing fixed said what the game itself does. Large reads (heap dumps, address
   lists) also filled the agents' working context and pushed the project's rules out of it.

4. What ends it: rule 11 (every test of the shell runs on the game's own code, read fresh from the user's copy)
   and scripts/game_test.ps1. The game test runs the real game read-only. It counts every call the game makes
   through its import table and records every "not provided" answer, where the run ended, frames and pipelines.
   Then it compares all of that with the last accepted run. The agents' commit tool refuses a change under shell/
   or scripts/ unless the game test passed on that exact build. First baseline (2026-11-26, 60 s): the game was
   alive at the end, it called 133 of its 587 imports (770,462 calls), none of them unprovided, 16,384 pipelines
   were made, and the shell gave 19 distinct "not provided" answers.

## 2026-11-26 - the game's own root signatures say where each shader finds its data (measured, game test 18:48)

ExCorvus: the shell has to be "the same as in the original exe"; what it provides is the rehydration of what the
game itself describes. So before anything else was changed, this run recorded the game's own root signatures
next to its shader modules (`WHIWHI_ROOT_SIGNATURE_RECORD`, device slot 16 and slot 86 in shell/d3d12_xs.cpp;
the record stays on the user's PC).

Game test 2026-11-26 18:48 (60 s, PASS, game folder unchanged):

- The game made 15 root signatures. The PC's own root-signature reader reads every one of the game's blobs as an
  ordinary version-1.1 root signature. Examples: one parameter that is a root buffer (t0, space 3); a root buffer
  plus two root constants at b500, space 0; a descriptor table with t266, u23 and b27 in space 1, plus 22 fixed
  samplers in space 1.
- 714 shader modules were named next to a root signature, every one next to a signature the game had made.
- Each entry of a module's first table names a root parameter, and the game's root signature has the matching
  kind of parameter at that number:
  - 710 entries of kind 0 (constants) name root constants at b500 with the same register number (500), and
    their count is the number of 32-bit values (2);
  - 2 entries of kind 1 (a buffer's address) name a root buffer (t0, space 3);
  - 2,591 entries of kind 3 (a descriptor table's address) name a descriptor table;
  - 704 entries of kind 6 (the sampler heap) and 6 of kind 5 (by number) name the number of a descriptor table;
  - kind 4 (fixed sampler) entries name a fixed sampler, not a parameter (their number is not a parameter of
    that signature, as the field's description in shader_program.hpp says).

So the game's own description is enough: a translated shader can take each resource from the register and space
the game's own root signature gives it, and the pipeline can use the game's own root signature. The shell's own
layout (one root signature of the shell's, an arguments buffer, heap places) is not needed. That is the next
change; nothing of it is built yet.

## 2026-11-26 - the shader tables checked by a second reader against the root signatures (measured, test run 19:05)

ExCorvus: the record of the entry above came from the translator's own reader, which was brought back from work
that went wrong, so it is not proof ("it introduces flawed patterns into the shaderlog"). The exe map settles one
half (docs/exe-map/SHADER-PATH.md section 4, verified): the program takes each pipeline's root signature from the
`PRIV` part of the shader container itself (fn 0xfa09f0, converted by fn 0x55ac630, made through device slot 16).
The other half - which register of a shader reads which root parameter - is never worked out in the exe file: on
the console it is done by the graphics library the exe imports, whose place the shell takes. The data for it is in
the two tables the console's shader compiler writes into each container.

So a second, minimal reader of those tables was written (shell/shader_check.*). It shares no code with the
translator, gives back the raw words of each table entry, and refuses with a reason whenever a step of the
description in shell/shader_program.hpp does not hold. With `WHIWHI_SHADER_CHECK=<file>`, shell/d3d12_xs.cpp
checks each claim of that description against the root signature the request names, as the PC's own reader of
root signatures reads it (on the user's PC, numbers only).

Test run 2026-11-26 19:05 (scripts/game_test.ps1, 60 s, PASS, the copy's folder unchanged; 714 pairs of a shader and its request's root
signature, 15 root signatures):

- Every one of the 714 containers was read by the second reader; no step of the description failed.
- Every claim about how a table entry names a root parameter held, with no exception: the parameter number
  (4,013 first-table and 1,994 second-table entries); constants (710 entries: same register, count = number of
  32-bit values; 453 declared constants: same register); root descriptors (2 + 1: same register); fixed samplers
  (14 + 5: number and register); descriptor tables (3,301 first-table entries name a table parameter).
- Not held: for a resource that arrives through a descriptor table, the table of the named root signature holds a
  range of that class with the shader's register for only **216 of 1,540**. For the other 1,324 the shader reads
  a register the named table does not list. Where it does list it, the shader's word 1 is 8 words times the
  register's position in the table for 190 of 216.
- The containers carry 6 different `PRIV` keys; each key came with only one root signature.

This corrects the conclusion of the entry above. The program's own root signatures are exactly right about the
kind, order and registers of their parameters, but what a descriptor table holds is, for most shaders, not what
the named root signature's table lists (measured). Why the console does not mind is inferred, not measured: its
shaders read a table by position, and each shader's own table says which position. Using the program's root signatures unchanged on the PC would therefore not give most shaders
their table resources. How the shell is to provide the table contents is not decided; nothing of it is built.

A second test run (2026-11-26 19:20, PASS, the copy's folder unchanged) checked the shaders' own tables on their
own, with no root signature and no heap memory read. For all 1,540 resources that come through a descriptor
table: word 1 is a whole multiple of the descriptor's size in words (the low byte of word 4); no two resources of
one table share a word 1; and the first table has a kind 3 entry with the same parameter, the same word 1 and the
same register. So each such resource has a position of its own in its table, said twice and consistently by the
shader's own tables (measured). That the console reads the table at that position is still inferred.

## 2026-11-26 - render-target views kept in the program's own heap memory (game test 19:46)

The program treats every descriptor as 32 bytes of its own memory at the address it chose for the heap, copies
those bytes as raw memory and never reads what is inside (docs/exe-map/DESCRIPTORS.md C.2/C.3). Until now the
shell kept descriptors only in the PC heap and left that memory empty. ExCorvus chose constant rehydration: the
program's memory is the record, and the shell turns it into PC objects as they are used. This is the first heap
done that way, the render-target heap (type 2), written by a helper agent and read through before the test.

- Device slot 20 (CreateRenderTargetView) still makes the PC view, and also writes a 32-byte note at the
  program's own address: a fixed mark, the kind and a view number. The note's layout is the shell's own (the
  program never reads it). The view number names a record of the resource and a copy of the description.
- The description the program passes is the public 24-byte one (measured at the exe's one call site,
  fn 0xcc2960, call at 0xcc29e4: 24 bytes zeroed on the stack, then format, view kind and the kind's fields).
- Copies (device slots 23 and 24) of type 2 also copy the notes in the program's memory; the sources are read
  before anything is written.
- Where a render-target handle is used (list slots 50 and 52), the note is read first; if the PC position does not
  hold the view it names, the view is made again there from the record.

Game test 2026-11-26 19:46: PASS, alive after 60 s, the copy's folder unchanged, no new missing answers, a
render-target clear reached the PC list. The last frame token was 108 against 116 in the accepted run; one run
does not tell whether that is variation.

Open, to be fixed next (ExCorvus, 2026-11-26): (1) the records only grow - every slot-20 call adds one and none
is removed; (2) a record keeps the resource without a reference, so a remake after the resource is gone would use
a stale one; (3) a copy from a position without a note writes zeros instead of the source's bytes; (4) the remake
almost never happens, because the PC copy already carries the view across.

## 2026-11-26 - render-target notes: the four open points fixed (game test 20:06)

The four points left open in the entry above, fixed as ExCorvus decided (written by a helper agent, read through
and corrected once before the test):

1. Each record counts the notes in the program's memory that name it. A note is added by device slot 20 and by a
   copy, and removed when its 32-byte place is overwritten or its heap is released (the heap's memory is walked
   for notes before it is given back). At zero the record is freed and its number used again.
2. A record holds a reference on its resource while notes name it, and gives it back when it is freed, so a view
   is never made again from a resource that is gone.
3. A copy is a raw copy: the 32 bytes move as they are, whatever they hold.
4. The notes are the only record: a render-target copy between two heaps with program memory is no longer passed
   to the PC library. The destination's PC position is cleared, and the view is made there from the note when a
   binding or a clear uses it. Where one side is not in such a heap, the PC copy is passed on as before.

Game test 2026-11-26 20:06: PASS, alive after 60 s, the copy's folder unchanged, no new missing answers, a
render-target clear reached the PC list, last frame token 122 (accepted run 116).

Not changed (rare cases, written down): a view being made again while another thread overwrites the last note
naming it could let its resource go during the remake; a held resource can outlive the program's use of its
memory, which the program may reuse - using such a view would be the program's own late use.

## 2026-11-26 - render-target and depth views in the heap memory the program was given; the notes dropped (game test 20:25)

The notes of the two entries above were a layout of the shell's own, and they kept a second heap next to the
memory the program was given, with machinery to keep the two in step. The program asks for none of that: it
expects the 32 bytes at its own address to be the view. The notes and their four fixes are dropped (the graphics
library is back to its state at commit 699 for them), and the views now live where the program expects them.

The layers sit inside each other: the program calls its graphics library, which calls the PC library, which
writes into the heap memory the memory library handed down for that heap. Measured in the PC library the user
builds (vkd3d-proton 71cae09; read, nothing copied): a render-target or depth handle is a plain address; the view
is a 32-byte record written at that address; copies of these heap types are a plain byte copy; the view itself
belongs to its resource, so the record holds no count of its own. So for heap types 2 and 3 the graphics library
now hands the program's own address down unchanged instead of moving it into a heap of its own. This is a
measured fact of that PC library version, not something Direct3D 12 promises.

Game test 2026-11-26 20:25: PASS, alive after 60 s, the copy's folder unchanged, no new missing answers, a
render-target clear reached the PC list, last frame token 119. Measured in the run: 21 render-target and depth
views made, all 21 found filled in the program's own heap memory, none empty.

## 2026-11-26 - which memory the program holds for the graphics side (measured, game test 20:48)

The memory library keeps every region the program asked for, with the protection it gave. A count at the end of
the run (the graphics library's end-of-run report, numbers only) groups them. The bits above the low byte of the
protection are Windows' own public page flags (winnt.h, SDK 10.0.26100: PAGE_GRAPHICS_NOACCESS 0x800, _READONLY
0x1000, _READWRITE 0x2000, _EXECUTE_READ 0x8000, _COHERENT 0x20000, _NOCACHE 0x40000; PAGE_WRITECOMBINE 0x400).
Held at the end of the 60-second run:

- the two 24 GiB views: processor read-write (0x4), and graphics no-access (0x801) with no pages mapped yet;
- graphics read-write (0x2004): the 6.5 GB streaming pool, a 100 MiB and a 30 MiB pool;
- graphics read-write, write-combined (0x2404): 2.3 GB and 49 MB;
- graphics read-write and coherent (0x22004): three pools, 2.4 GB together;
- graphics execute-read, write-combined (0x8404): 28 MB (inferred: shader code);
- graphics read-only (0x1004, 0x1404): 11 MB; graphics read-write, write-combined, no-cache (0x42404): 64 KiB;
- processor only (0x4): one 20 MiB pool; and through XMemAlloc (protection 0x4) the five descriptor heaps
  (attributes 0x00800004) and the display surface (0x00808000).

The shell's memory library uses only the low byte (processor access) when it asks Windows; none of the memory is
reachable by the graphics card today. The coherent flag is where the graphics side writes memory the processor
reads at once: free on the console's shared memory, a property of the memory on a PC (ExCorvus: "a precise flag
where the pipeline injects into the cpu").

## 2026-11-26 - the processor part of the program's memory flags passed on to Windows (game test 21:00)

The memory library used only the low byte of the protection the program gives (the processor's access). Asked
directly on the user's PC (VirtualAlloc reserve+commit and MapViewOfFile3 of a page section, 64 KiB each, no
program involved): Windows accepts PAGE_READWRITE with PAGE_WRITECOMBINE (0x404) and with PAGE_NOCACHE (0x204),
for committed memory and for mapped pages; it refuses every PAGE_GRAPHICS_* flag the program uses (0x801, 0x1004,
0x1404, 0x2004, 0x2404, 0x8404, 0x22004, 0x42404) with error 50, not supported. The memory library now passes
write-combined and no-cache on as the program asked; the graphics bits stay in the region record only. The
sample test that expected write-combined memory to come back as plain read-write was changed to expect it
write-combined.

Game test 2026-11-26 21:00: PASS, alive after 60 s, the copy's folder unchanged, no new missing answers, no
fault and no watchdog stop. The last frame token was 1,610, against about 120 in every run before (about 29
frames a second instead of about 2). Pipelines made in the run: 4,096 against 16,384. Why fewer pipelines are
made in the same time is not known (open). ExCorvus: the watchdog checks the loop's pace (measured in the map:
one-second passes, 30 ticks re-armed by progress, so about 30 seconds without progress stops it), and a program
that runs slower than it should skews boot results - earlier frame counts are not comparable with this one.

## 2026-11-26 - the program's progress reports, read during a run (game tests 21:08 and 21:09)

The boot tool now reads, every half second and without writing anything, the time of the program's last progress
report (image+0x71b6338; written by one function of the program; its watchdog thread compares it once a second,
and when about 30 passes go by without a new one - 300 s while a watch scope is open - the program stops itself,
docs/exe-map/MAP.md sections 2.2 and 4). The game test lists the number of new reports and the longest time
without one.

- 60-second run (21:08): PASS; 1 new report, about 6 s before the end; last frame token 1,581; 4,096 pipelines.
- 150-second run (21:09): PASS, alive after 150 s; still 1 new report, then 96.6 s without another, and the
  program did not stop itself; last frame token 1,561 (about the same as after 60 s); 26,310 pipelines.

So around the first minute the program reports progress once, its frame count stops rising, and it keeps making
pipelines while it allows itself more than 30 s. That a watch scope is open then (the 300 s budget) is inferred
from the map, not measured. Not chased further (written down for ExCorvus).

## 2026-11-26 - device slot 70: buffer views of the program's own memory (game test 21:29)

Slot 70 is (device, block A, block B, destination) (docs/exe-map/DESCRIPTORS.md B). Block B's first 0x28 bytes are
the public D3D12_SHADER_RESOURCE_VIEW_DESC (measured with the compiler on the SDK header; the map was corrected),
and +0x28 is the resource's GPU address. For a buffer (block A type 1, size at +0x10) the graphics library now
opens the memory at that address on the PC device as it is (as for slot 133, nothing copied), places a PC buffer
over it from the 64 KiB boundary below, and makes the view with the program's own public description, its first
element counted from that boundary. Texture views (block A types 2-4) are logged and not answered yet.

Game test 2026-11-26 21:29: PASS, alive after 60 s, the copy's folder unchanged, slot 70 no longer "not
provided". Buffer views made, for example 182,452,224 bytes (format 39), 6,144 and 768 bytes (format 57, the
second with first element 3,072 after the shift), 2,048 bytes of one 2,048-byte structure. Open: one buffer of
2,048-byte structures lies 0x6200 bytes after a 64 KiB boundary, not a whole number of structures, so no view was
made for it (the PC places buffers only on 64 KiB boundaries).

## 2026-11-26 - which memory the descriptor heaps lie in, and what vkd3d-proton needs around a heap (game test 21:54)

The graphics library now logs, when a descriptor heap is made, what Windows reports for the page at the heap's
address and which of the memory library's regions hold it. Game test 2026-11-26 21:54: PASS, alive after 60 s,
the copy's folder unchanged. The large shader-visible heap (1,245,184 descriptors of 32 bytes) lies in **mapped**
memory (Windows type MEM_MAPPED), inside no single region of the memory library. The other heaps lie in private
memory from XMemAlloc. Windows reports writes to memory (GetWriteWatch) only for private memory that was reserved
for it, so it cannot report the program's writes into the large heap.

Measured in vkd3d-proton 71cae09 and on this PC: the build here runs the VK_EXT_descriptor_heap path ("VK_EXT_descriptor_heap
enabled (32 / 16 size)"). For a shader-visible resource heap it keeps a 64-byte guard area in front of the first
descriptor (the heap's CPU start is its memory + 64; the log's PC heap addresses end in 0x40) and, behind the last
descriptor, one null descriptor and 4,096 descriptors for its own internal work, all in the same memory, and it
writes into them. The program's heap memory is exactly the size it asked for, and the memory around it is the
program's, so vkd3d-proton cannot use that memory as its heap as it is. ExCorvus chose to mirror the program's heap
into vkd3d-proton's heap at the border instead (no vkd3d-proton change for this).
## 2026-11-26 - the device byte watched in two visible runs, and the installation's own libraries (game tests 22:07, 22:09, 22:36)

Two runs with the window shown (game_test.ps1 -Show), one with the device byte at +0x18 set to 0 for that run only
(22:07) and one with the committed 1 (22:09). Both passed: alive after 60 s, the copy's folder unchanged, frame
tokens 1,591 and 1,579, 4,096 pipelines each. ExCorvus watched: with 0 the window was white, with 1 it was black.
In both runs the log shows exactly one picture presented to the window, at the very end of the run; for the rest
of each run the window held nothing from the program, so the white and the black are not the program's picture.
The log does not record the calls in which the two settings differ (device slots 17 and 24). Why the two windows
differ is not measured.

ExCorvus: a test has to run with the program's whole installation, not with libraries left out. Measured, from the
files only: the executable imports 68 libraries; six of them ship in the installation (XboxBvhBuilder.dll,
Bink2Scarlett.dll, amd_fidelityfx_loader_xbox.dll, libHttpClient.GDK.dll, MSVCP140.dll, VCRUNTIME140(_1).dll).
Everything those shipped libraries import is present: the shell's d3d12_xs.dll and xmem.dll export the functions
they ask for (XboxBvhBuilder: D3D12SerializeVersionedRootSignature and XMemVirtualQuery; Bink2Scarlett: XMemAlloc,
XMemFree and Windows' own XAudio2_9.dll, which is in System32; the FidelityFX upscaler: D3D12CreateRootSignatureDeserializer,
XMemAlloc, XMemFree, XMemGetWorkingSetStatistics). Until now the shell put its own XboxBvhBuilder.dll (a skeleton
that answers failure) in place of the installation's, with the stated reason that the shipped one cannot load on
a PC; that reason did not hold. The exe map (GRAPHICS-CALLS.md section 5, item 7) already said it is to be loaded
unchanged against the shell's device and memory libraries.

Changed for the run at 22:36 (not committed: the run failed): host_libraries.cpp logs, once per library, the file
it was loaded from; game_test.ps1 leaves out of the run folder every shell file whose name the installation ships
itself. Result of that run (FAIL, 5 s, the copy's folder unchanged):
- every import connected (587 of 587). The shipped XboxBvhBuilder.dll, amd_fidelityfx_loader_xbox.dll,
  libHttpClient.GDK.dll and Bink2Scarlett.dll were loaded from the installation. MSVCP140.dll, VCRUNTIME140.dll and
  VCRUNTIME140_1.dll were loaded from C:\Windows\System32, not from the installation (why is not measured).
- the run ended at the step where, in the passing run 22:09, the program constructs its ray-tracing builder (right
  after the second 3840 x 2160 surface). Windows recorded an application crash of boot_image.exe in KERNELBASE.dll
  with code 0xe06d7363 (a C++ exception that nothing caught). No device call is logged between the surface and the
  end. Which code threw it, and what it was given, is not measured.
- Windows also recorded two LiveKernelEvent 141 reports (a graphics hang report) at 22:36:40. Their attached dumps
  are named WATCHDOG-20261125-2149 and -2150 (the day before), and the System log holds no driver event from these
  minutes; so they look like older reports sent now, not a failure of this run. Not established.

Also noted, not followed: the tool connect_imports now ends with a Windows heap-corruption stop (0xc0000374)
before printing its report, with and without the installation folder.

Follow-up, game tests 22:39, 22:40, 22:42 (all FAIL at the same step, the copy's folder unchanged; recording only,
nothing answered differently): boot_image.cpp now records every C++ exception (its type names, a number's value,
and the calls that led to it) and every text written for a debugger (OutputDebugString); d3d12_xs.cpp records a
refused D3D12SerializeVersionedRootSignature (the description's parts and d3d12.dll's reason), the answer staying
d3d12.dll's.
- What is thrown: a `long`, value 0x80070057 (E_INVALIDARG), thrown inside the installation's XboxBvhBuilder.dll,
  called from the program's construction of the builder (returns to image+0x2479455).
- Why (the builder's own code, read from the copy: the path that ends in the throw follows a failed
  D3D12SerializeVersionedRootSignature): the builder describes a root signature of version 1.1 with three
  parameters (a shader-resource view, an unordered-access view, 16 constants, all registers 0, visible to all) and
  root signature flags 0x3000000. d3d12.dll refuses it: "Unsupported bit-flag set (root signature flags 3000000)".
  The builder then writes "D3D12 XDXR Error: Unexpected internal Failure:
  D3D12SerializeVersionedRootSignature(&desc, &pRootSignatureBlob, nullptr)" and throws.
- The two bits 0x1000000 and 0x2000000 are not among the PC's root signature flags. They are console additions to an
  otherwise public description. What they mean is not known: nothing in the executable or the builder reads them
  back; they are for the console's own graphics library.

Follow-up, game tests 22:47 and 22:48 (both stop about 5 s in; the copy's folder unchanged):
- The installation's builder carries the same two bits in the root signatures inside all 52 of its own shaders
  (measured: every RTS0 part of the 52 shader containers in XboxBvhBuilder.dll has flags 0x3000000). The bits were
  part of how those shaders were built, not something the builder adds on its own.
- vkd3d-proton writes a root signature description with these flags unchanged and, when it is created, logs
  "Ignoring root signature flags 0x3000000" (its source: unknown flags are noted, not refused). With the
  serializer taken from the same library as the device (d3d12_vk.dll), the builder's root signature is created
  (22:47). The next stop was device slot 62, which the shell did not provide.
- Device slot 62 (public description D3D12_COMPUTE_PIPELINE_STATE_DESC, docs/exe-map/SHADER-PATH.md) now makes a
  compute pipeline on the PC device when the shader is a plain PC shader (DXIL, no console part). At 22:48 the
  PC device refuses the builder's first one (5,136 bytes) with E_FAIL; vkd3d-proton's shader converter
  (dxil-spirv) logs "Unhandled instruction!". The builder throws that error.
- Why (dxil-spirv source, and the builder's 52 shaders read by an own reader of their DXIL bitcode): dxil-spirv
  reads every LLVM instruction kind these shaders use except one, `insertvalue` (record 27). Five of the 52 shaders
  use it (13, 8, 16, 16 and 64 times); the refused one has 13. The newest dxil-spirv on GitHub (b540c8a, the same
  commit vkd3d-proton uses) does not read it either.
- Not changed: nothing of the builder; the shaders are given to the PC device as they are.
## 2026-11-26 - the builder's shaders call the console's own shader functions (game test 23:13, measured)

- vkd3d-proton built from the fork (E:\vkd3d-fork, branch descriptor-heap-over-host-memory) with one change in its
  shader converter dxil-spirv: it now reads the LLVM instruction `insertvalue`. In the game test 23:13 (stops about
  5 s in, the copy's folder unchanged) the builder's first shader passes that point. dxil-spirv then stops with
  "Function 'xbox.global_store_dwordx4_with_cache' is declared but has no definition", and the builder throws
  E_INVALIDARG from device slot 62.
- Why (the builder's 52 shaders read from the copy with an own reader of their DXIL bitcode, names of the functions
  each one declares but does not contain): beside Microsoft's public `dx.op` functions, the shaders call 33
  functions whose names begin with `xbox.`. They are named after AMD GPU instructions and operations: bit-field
  insert, three-way minimum/median/maximum, lane id, ballot, lane read and swizzle, conditional mask, byte align,
  packed 16-bit minimum/maximum, bit count, loads and stores at a GPU address "with cache", scalar loads, atomic
  add/sub/fmin/fmax at an address, a typed load. 17 of the 52 shaders use `xbox.global_load_dword_with_cache`.
- So the builder's shaders are DXIL with console functions in them, not plain PC shaders. On the console these
  functions are supplied by the console's shader compiler in its graphics library. What each function's arguments
  mean is not established; only the names and the types are read.
- This corrects the earlier note (22:39) that the builder's shaders are "plain PC DXIL".- Follow-up (same day, the builder's 52 shaders read again): every `xbox.` function is called with one fixed first
  number, an operation number like the one Microsoft's public shader functions take. These numbers are not
  Microsoft's public operation numbers (bit-field insert is 23 here and 53 in Microsoft's list; the address loads,
  stores and atomics are 355 to 369, beyond Microsoft's list): the console numbers them itself, and the names say
  what they do. 64-bit lane masks are passed as pairs of 32-bit halves, which is what the shaders build with
  `insertvalue`. Loads, stores and atomics take a 64-bit GPU address; the two "cache" inputs of every load and
  store are left undefined at every call, so no cache setting is passed. Where these functions' code lives on the
  console (the console's own graphics library) is not in any file of the installation that may be opened.
## 2026-11-26 - the shell is called the weave (game test 23:50)

ExCorvus's find (ATTRIBUTION.md item 14): by its code the shell is a shell, but it is not wrapped around the
executable, it is woven all through it. The project now calls it a shell-based weave. The folder `shell/` is now
`weave/` (build: `weave\build.cmd`), the code's namespace is `whiwhi::weave`, and the word is changed in the code,
its comments and log texts, the scripts and the docs. Windows' own word PowerShell is unchanged. ExCorvus's own
words keep the word she used, and so do the dated entries before this one: there, `shell/...` paths mean `weave/...`.

Check (visible game test 23:50 on the renamed build, the copy's folder unchanged): the run stops at the same place
as the run of 23:13 - the same throw inside the installation's XboxBvhBuilder.dll after device slot 62, through the
same calls into it from the program, with the same counts in the summary. The log differs in one line only, a
measurement that was switched on at 23:13 and not at 23:50, apart from the boot tool's own code offsets, which moved
with its changed texts. The rename was one commit (735), by ExCorvus's exception to one file per commit and to the
game-test gate; the docs followed one file per commit (736-753).
## 2026-11-27 - how many lanes the builder's shaders were made for, and what the PC side does with it (measured, no run)

Read from the installation's XboxBvhBuilder.dll (read-only; numbers only, tools in the agent's temp folder):
- All 52 shaders are compute shaders. Their PSV0 part leaves the lane count open (minimum 0, maximum any); none
  declares a WaveSize the PC way.
- Every shader carries one named metadata entry a PC shader does not have, `xbox.compiler.options`: the options
  the console's shader compiler (`dxcxbox 2504.2509.11601.10011`) was given. In 42 of the 52 it holds the switch
  `__XBOX_ENABLE_WAVE32`. All 35 shaders with thread groups of 32 have it, among them the one the PC device refused
  at 23:13 (5,136 bytes); so do six of the seven with groups of 1,024 and the one with a group of 1. The ten without
  it have groups of 64 (4), 256 (1), 512 (4) and 1,024 (1).
- So the "64" in names such as `xbox.Ballot64` is the width of the answer (a 64-bit lane mask), not the lane
  count. Which lane count a shader without the switch runs with on the console is not written in it; that it is
  64 follows only from the switch's name (inferred).
- Her card (vulkaninfo): subgroup size 64 by default, 32 to 64 possible, and a compute shader can require one
  (subgroupSizeControl, computeFullSubgroups, requiredSubgroupSizeStages includes compute).
- vkd3d-proton (libs/vkd3d/state.c, the stage setup for a shader that uses lane operations): with no WaveSize in
  the shader it allows a varying lane count and leaves the choice to the driver. Nothing reads the console's
  switch, so whether a shader made for 32 lanes runs with 32 is not decided by anything that knows what it was
  made for.
- Not changed: nothing. The step this points to, taking the console compiler's switch as the lane count the
  shader requires, is for ExCorvus to decide.
## 2026-11-27 - the lane count taken from the console compiler's switch (game test 00:04)

- vkd3d-proton's shader converter (dxil-spirv, in ExCorvus/dxil-spirv, not yet committed there) now reads the
  console compiler's options that every builder shader carries (`xbox.compiler.options`). When a shader has no lane
  count of its own (no WaveSize tag), it asks for 32 lanes where the options hold `__XBOX_ENABLE_WAVE32` and 64
  where they do not. Her card offers both (vulkaninfo: subgroup size 32 to 64, size control for compute).
- Game test 00:04 (visible, the copy's folder unchanged): FAIL, the same stop as 23:13 and 23:50. The first shader
  the builder hands over (5,136 bytes) is refused at a console memory function,
  `xbox.global_store_dwordx4_with_cache`, which the converter does not know. The run ends before its report.
- So this run cannot show whether the lane count is taken: the converter stops before the shader is finished.
  Not shown either way; left as it stands.
Follow-up, game test 00:12 (visible, the copy's folder unchanged; FAIL about 5 s in):
- The converter now writes the two console stores the first builder shader uses
  (`xbox.global_store_dword_with_cache`, `xbox.global_store_dwordx4_with_cache`: 4 or 16 bytes to a GPU virtual
  address; the last two arguments are taken and not interpreted). With them that shader (5,136 bytes) is accepted:
  device slot 62 answers 0x0, a compute pipeline made by the PC device. That the pipeline does what the console's
  does is not shown: it has not run yet.
- The builder then hands over its next shader (7,876 bytes). It uses two console loads,
  `xbox.global_load_dwordx3_with_cache` and `xbox.global_load_dwordx4_with_cache`; the converter does not know
  them yet, the device answers 0x80070057 and the builder throws. That is the new stop.
- The lane count change is in the same converter; this run still cannot show whether it is taken.
## 2026-11-27 - all builder shaders through the translator at once, and the next stop (game test 00:22)

- A checker outside the repository (C:\Users\fabiq\AppData\Local\Temp\opencode\bvh_check) reads XboxBvhBuilder.dll
  from the copy into memory, finds its 52 shader containers and passes each through dxil-spirv as vkd3d-proton does
  (buffers by GPU address, root constants as push constants). Nothing of the file or of the translated code is
  written anywhere. Before any change it agreed with the game test of 00:12 on both shaders that run reached
  (the 5,136-byte one accepted, the 7,876-byte one refused for the 16-byte load).
- Taught in the fork's dxil-spirv after that: the loads of 4, 8, 12 and 16 bytes and the constant load, the
  12-byte store, atomic add and subtract, float atomic minimum and maximum, Min3 / Med3 / Max3 (float and
  unsigned), BFI, V_ALIGNBYTE_B32, V_PK_MIN_F16 / V_PK_MAX_F16 and S_BCNT1_U64. The float atomic minimum and
  maximum need VK_EXT_shader_atomic_float2, which vkd3d-proton did not turn on; the fork now turns it on when the
  card has it (this card: shaderBufferFloat32AtomicMinMax = true).
- Checker: 41 of 52 accepted. The 11 others stop at lane functions (LaneSwizzle 7, MakeUniform 3; held, they
  depend on the lane count) and one typed load. "Accepted" says the translator understood a shader, not that the
  shader computes what it should.
- Game test 00:22 (run-20261127-002228; FAIL about 5 s in, the copy's folder unchanged): four builder pipelines are
  made (5,136, 7,876, 5,504 and 18,748 bytes, all 0x0), the second of them the one that stopped the run of 00:12.
  Then an access violation: execution at an address in ordinary memory (the bytes there look like a pointer, not
  code), called from 0x7ffd2920ae8a. That address lies in none of the libraries whose load the log names; which
  library it belongs to was not recorded. Not chased (it may be a binding symptom, rule of 2026-11-26).
Follow-up, game test 00:24 (run-20261127-002450; boot_image now names the library each stack word near a fault
points into): the same four builder pipelines are made, then an access violation inside AMD's Vulkan driver: a
read at an address made of text bytes, at an instruction whose stack words point only into amdvlk64.dll
(+0xfdae8a, +0xfdad80, +0xfce7cf). +0xfdae8a is the calling address of the run of 00:22, so both runs fail in the
same place of the driver, while the fifth pipeline is being made. The driver's own process failed; the machine and
the display stayed up. Which shader the fifth pipeline holds, and whether the translated code handed to the driver
is valid, was not measured.
Follow-up, game test 00:28 (run-20261127-002835): the same build, but the translator without the lane count taken
from the console compiler's switch (that change was held and not committed, but it was in the translator of the
runs of 00:04, 00:12, 00:22 and 00:24). Measured: no fault in the driver. 20 builder pipelines are made, among them
the fifth one (29,996 bytes) during which the driver failed twice with the lane count set. The run then stops where
expected: the 21st shader (8,188 bytes) uses MakeUniform, a lane function not taught yet, and the builder throws
E_INVALIDARG. So the driver failure came with the lane count the translator asked for: with it, the driver
failed; without it, the driver made the pipeline. Why, was not measured. The lane-count change stays held.
Follow-up, lane functions (game test 00:40, run-20261127-004049; FAIL about 5 s in, the copy's folder unchanged):
- Taught in the fork's dxil-spirv, all working from the lane number and lane count the driver chose (none assumes
  32 or 64): GetLaneID, ReadLane, MakeUniform (i32, i64), LaneSwizzle, V_CNDMASK_B32, Ballot64, MBCNT64,
  WaveMin_U32. How LaneSwizzle and V_CNDMASK_B32 are called was read from the shaders (in memory, to the screen
  only): LaneSwizzle gets the patterns 30, 60, 120, 240, 480, which as AMD's ds_swizzle bit-mask encoding read
  lane&~1, (lane&28)|1, (lane&24)|3, (lane&16)|7 and lane 15 within each 32 lanes; each is followed by
  V_CNDMASK_B32 with the lane masks 0xAAAAAAAA, 0xCCCCCCCC, 0xF0F0F0F0, 0xFF00FF00 (both halves) and the values
  {0, 1}. Together these are the steps of a running sum across lanes, so the reading holds for both (inferred
  from use).
- Checker: 51 of 52 accepted; the one left uses TypedLoad.
- Game test: 31 builder pipelines made, none refused. Then the driver fails again at amdvlk64.dll+0xfdae8a (the
  same place as at 00:22 and 00:24) while the 32nd is made. The translator set no lane count in this run. So that
  place in the driver is not reached only through the lane count. Which shader the 32nd pipeline holds was not
  recorded.
Follow-up, the typed load (game test 00:49, run-20261127-004956; FAIL about 5 s in, the copy's folder unchanged):
- xbox.TypedLoad.i32 has the shape of dx.op.bufferLoad (handle, index, a result of four values and a status). In
  the one shader that uses it, the handle is a raw buffer (DXIL resource kind 11) passed by GPU address and the
  last argument is 65 at both calls. The formats the program uses are the PC's DXGI numbers (28, 39, ... in every
  log), and DXGI 65 is A8_UNORM, one byte; the index is built as "x3 + ..." and "x9 + ...", and the shader keeps
  only the low four bits of the result. Taught as: the byte at byte offset index, as an integer in the first
  value (inferred from these fits, not written anywhere); other formats and buffer kinds are refused.
- Checker (its root-constant limit raised from 16 to 64 words, a limit of the checker only): 52 of 52 accepted.
- Game test: 31 builder pipelines made, none refused, then the driver fails in the same place as at 00:40, while
  the 32nd is made. So the driver failure does not depend on which builder shaders the translator understands.
Follow-up, which shader (game test 00:54, run-20261127-005424; the weave now names each shader before its pipeline
is handed to the PC device, by size and the first 8 bytes of the container's checksum):
- The 32nd pipeline, during which the driver fails, holds the builder's largest shader (74,508 bytes, position 20
  of 52 in the file). The failure lies in the driver's own program code while the pipeline is made, before
  anything runs on the GPU (an access violation in amdvlk64.dll reading an address made of text bytes).
- Its translated code asks for no Vulkan feature that the 31 pipelines made before it did not ask for (Shader,
  Int64, GroupNonUniform, its arithmetic and ballot parts, the Vulkan memory model, physical storage buffers, float
  atomic minimum/maximum; each is used by at least one of the 31, all of which the driver made). The driver reports
  every one of them as supported.
- What sets it apart: its translated code is 34,003 words long, more than twice the largest of the 31 (16,304).
  Whether size or something in its content leads the driver's compiler into the fault was not measured.
Follow-up, 2026-11-27 ~01:30 - the driver fault reproduced outside the program (a test tool, not in the repository):
- A small tool reads the builder library fresh from the copy into memory and makes its compute pipelines the way
  the builder does in a game run: one device of the same graphics library build (the fork's, from 00:49), the
  builder's one root signature (as recorded at 23:07: version 1.1, flags 0x3000000, root SRV, root UAV, 16
  constants), the shaders in the order the builder made them at 00:54 (32 measured, the other 20 in file order -
  the tool's choice). Nothing is dispatched; nothing of the shaders is written anywhere.
- Result, seven runs: the first 4 pipelines are made; making the 5th (29,996 bytes) faults in the driver at
  amdvlk64.dll+0xf0a3d4, reading address 0x6e6f69746f5d - the same instruction and the same address as in the game
  runs (00:24 on the 5th; 00:40, 00:49 and 00:54 on the 32nd). The address's bytes are text ("]otion"); that text
  is not in the builder library, its shader, the driver file or the graphics library file.
- Not the reason (each tried, same fault on the 5th): the root signature taken from each shader's own RTS0 part
  instead of the builder's (each shader's RTS0 part differs byte for byte from the builder's serialized one, same
  meaning not checked); feature level 12_0 or 12_1; a 64 MiB stack for the thread that makes them.
- Open: why the game runs since 00:28 get past the 5th and the tool does not. Differences left: what the program
  and the weave have made on the device before the builder, and the driver's own cache per program (guess).
Follow-up, 2026-11-27 ~01:15 - the driver fault found: a value the translator left unwritten (game test 01:13):
- The Vulkan SDK's validator (spirv-val, fed through a pipe in memory, nothing on disk) found 3 of the 52 translated
  builder shaders invalid: 29,996, 74,508 and 34,020 bytes - the one the tool faulted on, the one the game runs
  faulted on, and one not reached yet. In each, a calculation uses an ID that is never defined.
- That ID is the result of `llvm.copysign.f32(1.0, x)` (the sign of x as +1/-1; the shader takes 0 where x is 0).
  It is a plain LLVM function, not a console one. dxil-spirv passed over every `llvm.` call without a word (written
  for markers like llvm.lifetime that compute nothing), so the result was never written and the driver read
  whatever lay in memory - text, taken as an address.
- Changed in the dxil-spirv fork (not yet committed there): llvm.copysign.f32 is translated (the magnitude bits of
  the first value with the sign bit of the second); an `llvm.` call that produces a value and is not known is now
  refused with an error instead of passed over. Validator afterwards: 52 of 52 valid.
- The test tool (one device, builder order, each shader's own root signature): 51 of 52 made, no fault. The one
  refused is 38,248 bytes: xbox.TypedLoad on a buffer that its root signature hands over through the descriptor heap,
  where the translation taught so far covers only a buffer passed by its GPU address. (The tool's first attempt with
  the builder's one recorded root signature was wrong: the builder has more than one; 37 shaders use buffers it
  lacks.)
- Game test 01:13 (visible, the copy's folder unchanged): FAIL - the builder makes 48 pipelines without a fault (up
  to here 31) and stops at the 49th, that same 38,248-byte shader, refused the same way; the builder throws.
## 2026-11-27 - two changes to the shader translator offered upstream

ExCorvus decided to offer the two changes that are not Xbox-specific to dxil-spirv itself (the shader translator
inside vkd3d-proton), each with a full description of the problem, how it showed up, the change and how it was
tested:
- https://github.com/HansKristian-Work/dxil-spirv/pull/384 - `llvm.copysign.f32`, and LLVM helper calls that
  return a value are refused instead of skipped (the skip left a value unwritten and crashed the AMD driver).
- https://github.com/HansKristian-Work/dxil-spirv/pull/385 - `insertvalue`, with the fix for `extractvalue` on a
  structure that has no meta entry.
Both branches are built on upstream's newest state (b540c8a) and build there. Both texts say what is not shown:
the shaders pass the validator and their pipelines are made, but nothing of them has run, so correct results are
not checked; and the public compiler produces neither case, so no test of upstream's usual kind is added. The
Xbox compiler's own functions and the vkd3d-proton float-atomics change are held back until the builder runs.
## 2026-11-27 - the checker's "52 of 52 accepted" was a false pass (measured)

The shader checker (a tool outside the repository that runs the builder's 52 shaders, read from the copy in
memory, through the translator) handed every buffer of every shader to the translator as passed directly by GPU
address. That was the checker's own simplification, not what the shaders say. `xbox.TypedLoad` was then taught
only for that case, and the checker reported 52 of 52 accepted. The shader that uses it (38,248 bytes) takes its
buffer through a descriptor table in its own root signature, inside the builder library. With the real root
signatures (the driver test tool, and the game test at 01:13) it is refused. So the 52 of 52 held only because of
the checker's simplification: a pass made by a stitch in the test, not by the translator. No code of the weave is
involved; the refusal happens while the pipeline is made, before any descriptor exists.
Next (ExCorvus agreed): the checker takes each shader's own root signature, and is run again to see whether other
shaders were also passed only by the simplification; then TypedLoad is taught the descriptor-table path.
## 2026-11-27 - the builder's 52 shaders made with their own root signatures, and the first passing game test with the installation's builder (game test 01:34, measured)

- The driver test tool (outside the program; it reads XboxBvhBuilder.dll from the copy in memory and hands every
  shader, in the builder's own order, with the shader's own root signature, to vkd3d-proton on one device) ran
  with Vulkan's validation layer on. Before the change below: 51 of 52 made, no message from the layer; the one
  refused is the 38,248-byte shader with xbox.TypedLoad, whose buffer (u0, a raw buffer) comes through the root
  signature's descriptor table, not by GPU address. The checker's earlier "52 of 52" had hidden that (entry above).
- A first attempt to read that buffer through the table was reported by the layer
  (VUID-VkShaderModuleCreateInfo-pCode-08737: the access's type did not match the buffer's) and made the driver
  fault in the tool. So the layer does report, and a translation the layer reports is one the driver may fault on.
  Cause: the translator's first pass, which decides how each buffer is declared, did not know TypedLoad.
- Taught in the translator (dxil-spirv fork, commit b046daf6; vkd3d-proton fork 0bb92ce7): the first pass counts
  TypedLoad as a read of a byte address, and TypedLoad reads the 32-bit word that holds the byte through the
  table's buffer. Tool afterwards: 52 tried, 52 made, nothing from the layer, no fault.
- Game test 01:34 (visible, all files committed): **PASS**. Alive 60 s, the copy's folder unchanged, frame token
  1,584, 4,096 pipelines, 17 calls answered "not provided", one new progress report (longest unchanged 6.0 s).
  The installation's own builder is constructed: 53 compute pipelines of its PC shaders made by the PC device.
- What this does not show: that the builder's shaders compute the right thing. None of them has run yet.
- Next stop seen in the same run: device slot 62 is asked for a compute pipeline of a console shader (1,264 bytes,
  with the console part), returning into a library outside the executable (address 0x7FFD91A179F2; which
  library is not identified - the run did not record library start addresses). Answered "not provided".
- Also in the log, as before: the weave's own shader translator refuses some pixel shaders ("a texel read whose
  values go to an instruction the pass cannot type"). Not looked at.
Correction, game test 01:39 (PASS, same counts in kind; frame token 1,473, 8,865 pipelines): the 1,264-byte console
shader for device slot 62 does **not** come from a library outside the executable. The address in the 01:34 line
was read inside the routine that logs only once, so it named that routine's caller, not the program's call. Read
at the call itself, it is the executable: image+0x247fe6d, right after the call at 0x247fe66 in fn 0x247fd30, the
program's own compute-pipeline path (docs/exe-map/SHADER-PATH.md, row 62). So the next stop is the program's own
compute shaders, which carry the console part (PRIV); the weave does not make those yet.
## 2026-11-27 - what the program's own compute shaders carry (game tests 01:44 and 01:46, measured)

- Device slot 62 now names, for each console compute shader, its parts and the numbers in the DXIL part's header
  (numbers only). In both runs every console compute shader (10 and more per run, fewer than 100) carries the same
  seven parts: SFI0, ISG1, OSG1, PSV0, HASH, DXIL, PRIV. There is no RTS0 part.
- The DXIL part states a compute program (kind 5) of shader model 6.6, magic "DXIL", DXIL version 1.6. Its header
  says the code is 4 bytes long, and those 4 bytes are the letters "NOBC". So the PC part does not carry the
  program's LLVM code: where it would be, the shader says "no bitcode". In the 1,460-byte example the DXIL part
  is 816 bytes; what the roughly 790 bytes between its header and "NOBC" are is not read yet.
- What this means: the PC part of these shaders is a description without the program. A PC translator, LLVM
  included, has nothing to translate in it. Where the program's code for these shaders is, is not known: the
  220-byte PRIV part is too small to be it by size alone, which is an inference, not a reading.
Follow-up, game test 01:50 (PASS): the roughly 790 bytes in the DXIL part are the same directory the graphics
shaders carry (weave/shader_program.hpp, docs/exe-map/SHADER-PATH.md). Read with the weave's existing reader, three
console compute shaders (1,356, 1,264 and 1,460 bytes) each have three entries (kinds 0x12, 0x0D and 0x07, of 16,
roughly 550-700, and 28 bytes); the 0x0D block is packed (525-674 bytes) and unpacks to a module of 1,435-1,675
bytes laid out like the graphics modules, with its number at +0x70 (the place seen before in a compute module).
In it stands the program in the console's instruction set: 35, 49 and 66 instructions (196-380 bytes), each walk
ending exactly on the end-of-program word. So the program's own compute shaders carry their code as console GPU
instructions, the same form as its graphics shaders, and no PC code.
## 2026-11-27 - the program's named root signatures do not describe where its shaders' table resources sit (game test 02:04, measured)

Asked before changing how translated shaders bind: can a translated shader use the root signature the program
names for its pipeline exactly as it is? For that, the named signature's descriptor table would have to hold, at the
place where the shader's own tables put a resource, a range of that resource's class. Two claims were added to the
table check (`WHIWHI_SHADER_CHECK`, weave/d3d12_xs.cpp) and measured in game test 02:04 (PASS, 60 s, the copy's
folder unchanged; 1,540 table resources of the program's vertex and pixel shaders):

- With the place counted in 32-byte records (word 1 / 8, how the program fills its tables, DESCRIPTORS.md C.2):
  994 checks held, 1,593 did not (a resource whose record size is 8 words is counted twice in these numbers).
- With the place counted in records of the resource's own size (word 1 / low byte of word 4): 4 held, 489 not.
- Unchanged from 2026-11-26: the named table lists the shader's register for 216 of the 1,540.

So the table ranges of the named root signatures are mostly not where the shaders' resources are, by register or
by place. What does hold for every shader is the part about parameters (their number, kind, the constants'
registers and counts, the root descriptors), measured on 2026-11-26. On the console, the program's shaders find
their table resources by their own tables (parameter and place), not by the ranges of the root signature.
## 2026-11-27 - the program's own root signatures in the weave's pipelines, first run (game test 02:23, measured)

Built (not committed: the test failed): the weave no longer uses a layout of its own for the program's graphics
pipelines. Each root signature the program's own code creates is made on the PC with the same parameters in the same
order, with each parameter's number as the register space of what its shaders read (a table's place k = register k);
the translated shaders name their resources that way, from their own tables; a pipeline uses the program's root
signature and is set on the PC list as the program names it. Root signatures a library creates (the installation's
builder) are made as given. 27 of 27 sample tests pass.

- **The graphics driver failed** (rule 7: written first): about 60 s in, boot_image.exe ended inside the AMD Vulkan
  driver (amdvlk64.dll +0xf22645, status c0000409, Windows' fail-fast stop; Windows Error Reporting, 02:24:00). The
  machine and the desktop stayed up. The last line before it is vkd3d-proton's "Invalid primitive topology type #13":
  a pipeline description names primitive topology type 13, which the public list (0 to 4) does not have. That
  message did not appear in the run before (02:04). The copy's folder is unchanged.
- The four root signatures the program created were made on the PC renamed (92, 116, 1304 and 1280 bytes).
- What the pipelines refused, each reason once: a vertex shader reads a register holding a table's or buffer's
  address as a number (s9); a sampler taken from a sampler table at a place computed at run time; a constant buffer
  at place 3 of a table whose root signature range has 2 places (also 2 of 2, 1 of 1); a resource taken from the
  descriptor heap by number; a fixed sampler. 2,052 pipelines were made (16,384 in the run before, made against the
  weave's own layout and never drawn).
- So, measured: the program's shaders read places past the ranges its named root signatures give their tables
  (as the 02:04 entry found from the other side); some shaders compute a sampler's place at run time; a vertex
  shader uses an address as a number. Nothing was changed after this run.
Follow-up, game test 02:33 (the same build plus one record: every pipeline description whose primitive topology
type is not one of the public 0 to 4; behaviour unchanged). The run ended the same way, about 58 s in, in the AMD
driver after vkd3d-proton's "Invalid primitive topology type #13". The copy's folder is unchanged.
- In the whole run one description names a type outside 0 to 4: type 19, near the end, from the program's own
  pipeline path (the call returns to image+0x2479a8d, fn 0x2479760). vkd3d-proton prints that number in hex: 0x13 is
  19, so its message is about this description (inferred from the number; that this one request was made, and not
  refused, is not shown by the log). All pipeline descriptions of the program have the same 15 subobjects (root
  signature, the five graphics shader stages with only the vertex and pixel ones filled, blend, sample mask,
  rasterizer, second depth-stencil, sample description, input layout, topology type, render-target formats,
  depth format).
- Read in the executable: the description is put together in the program's draw path (fn 0x32520, which calls fn
  0xb5a5d0 at 0x3275f). There, at 0x32710-0x3273e, the program turns the topology number set on the list into a
  class of its own for the pipeline's key: number 1 stays apart, 2 and 3 become 2, 0x11 stays 0x11, any other number
  below 0x20 becomes 4, and 0x20 and above become 0x20. 0x11 (17) is the topology number not on the public list
  that the program sets on its lists (seen in earlier runs; the weave holds draws while it is set). Where that class becomes the
  description's type 19 is not read (fn 0xb5a5d0 hashes the key and hands a request on; the request is filled
  elsewhere).
## 2026-11-27 - the program's topology numbers 0x11 and 0x13 are the AMD rectangle list and quad list (read in the executable)

- The renderer's state keeps the topology number at +0x1237d of its context object; 211 instructions in the
  executable read or write it (list in a local file). 39 places write 0x11 and 2 write 0x13; the others write the
  public numbers. The draw path (fn 0x32520) passes it to command-list slot 20 (IASetPrimitiveTopology) and sorts it
  into its own class for the pipeline key (previous entry).
- What the program draws with 0x11, read at one of the 39 places (0x124995): it sets 0x11, asks a vertex area of 3
  vertices of 24 bytes (call 0x517a290, 3 and 0x18), and writes three corners of the screen: (-1, 1), (-1, -1) and
  (1, -1), each followed by its other values; the fourth corner, (1, 1), is not written. Then it draws. Three corners
  of an axis-aligned rectangle, the fourth left to the hardware: that is a rectangle list.
- AMD's own numbering of hardware primitive types (public, in Mesa's register description
  src/amd/registers/gfx10.json, VGT primitive type): DI_PT_RECTLIST = 17 (0x11), DI_PT_LINELOOP = 18, DI_PT_QUADLIST =
  19 (0x13), DI_PT_QUADSTRIP = 20, DI_PT_POLYGON = 21. The program's two extra topology numbers are exactly AMD's
  rectangle list and quad list. The public D3D12 numbers it also uses (1 to 5, 10 to 13, 33 and above) are D3D's
  own. That the console's library passes 0x11 and 0x13 to the GPU as these hardware types is inferred from the
  numbers and from what the program draws with 0x11.
- The one pipeline description with topology type 19 (previous entry) therefore names the quad list (inferred:
  the same number, 0x13).
- On a PC: AMD's hardware has these primitive types, but Vulkan (and so vkd3d-proton) offers neither a rectangle
  list nor a quad list; the PC library has no number for them. Nothing was changed.
## 2026-11-27 - can the PC reach the GPU's own rectangle and quad modes? (measured, chain: the executable, AMD's public code, the installed driver)

ExCorvus set the order: first the executable, then AMD's public documents, then the installed driver itself.
- The executable: 37 functions set the topology number 0x11 (rectangles) or 0x13 (quads). 33 of them set nothing
  else and test nothing before it; in three more, other numbers follow as separate draws, not as a replacement. One
  (fn 0x5178b90) has a triangle path and a rectangle path side by side; what chooses between them is not read. So
  the program has no path without these two modes: it expects them to be drawn.
- AMD's public driver code (GPUOpen xgl, icd/api/include/vk_conv.h): the table that turns a Vulkan topology into
  the GPU's own lists exactly Vulkan's eleven topologies. None leads to a rectangle or quad mode.
- The installed driver (the one Windows names for the RX 9070 XT, amdvlk.inf_amd64_1d6da6c2496b97d2\amdvlk64.dll,
  driver 32.0.32029.2010), read only: its own list of Vulkan topology names has the same eleven. The word "RectList"
  appears many times inside the pipelines the driver keeps for its own work (copies and clears), so the driver uses
  the GPU's rectangle mode itself. Of the 244 extensions the driver offers (vulkaninfo), none is about rectangle or
  quad lists. The driver keeps its extension names in a form a plain text search does not find, so the offered list
  is taken from vulkaninfo, not from the file.
- Result: the GPU and its driver know rectangles; nothing in the PC's graphics interface reaches them from outside.
  On the PC they have to be drawn as triangles: quads by an index list, rectangles by a small extra shader that
  makes the fourth corner (v0 + v2 - v1).
## 2026-11-27 - rectangle and quad pipelines taken as triangle pipelines; the next stop is the audio device (game test 02:57, measured)

- Change (vkd3d-proton fork, libs/vkd3d/state.c): a graphics pipeline whose topology type is the Xbox Series
  rectangle-list (0x11) or quad-list (0x13) number is made as a triangle pipeline, with a warning in the log. The
  GPU rasterizes both as triangles. Draws with these topologies are still held by the weave (they need the
  topology turned into triangles when they are recorded: steps 2 and 3, not built).
- Game test 02:57 (run-20261127-025735, visible, the copy's folder unchanged): FAIL. "Invalid primitive topology
  type" is gone from the log and the AMD driver no longer ends the run. The program set pipelines on its lists
  (4 times) and asked for draws; draws while topology 0x11 is set were held (32 counted). 32 pictures were
  presented, frame token 1725. Calls reached for the first time: command list slots 44, 47, 48, 70, 75, 126, 142
  and device slot 81 (answered not implemented), and a console compute shader of 1376 bytes through slot 62.
- The run ends where the program stops itself: an int3 at image+0x43ad94e in its audio setup. The order in the
  log: ApuCreateHeap (answered, 477,102,080 bytes), AcpHalAllocateShapeContexts (350 contexts), ApuAlloc, then
  AcpHalCreate, which the weave answers "not provided" (E_NOTIMPL, no handle). The function the setup calls at
  that point (fn 0x43aec40) returns false and the next instruction is the int3. So the program does not go on
  without its audio device. Earlier runs ended or were measured before the program got this far.
## 2026-11-27 - the program's audio setup, read in the executable (first reading, measured)

ExCorvus: audio next, and "answering it silently might be a trap". So first what the program puts in and reads
back, before any answer. Read in fn 0x43aec40 (the setup) and the places that use the same globals.
- The executable imports nothing for sound except AcpHal.dll and xapu.dll (no XAudio2, no Windows audio). All of
  its sound goes through the audio device. (The installation's Bink library uses XAudio2 itself.)
- Order in fn 0x43aec40: the status word at 0x9ae98c8 is set to 0; AcpHalAllocateShapeContexts with a block at
  0x9ae98d0 asking for 350 (0x15e) records; the 350 x 64 bytes are zeroed; 350 flag bytes set to 1; a table of
  350 x 32 bytes at 0x9ae9938; ApuAlloc of 0x222e00 bytes (kind 2), cut into 350 pairs of 0x1700 and 0x200
  bytes (one pair per record); AcpHalCreate(&0x9ae98b8); the object's slot 0 with 0x1000, 0x1000;
  CreateEventA (manual reset, not set) at 0x9ae98c0; wait while the status word is negative (Sleep 10); then
  the object's slot 0x10 with opcode 2, 0xf0f0f0f0, -1, a 4-byte block holding 0xc0, 0. The setup succeeds when
  that last call is not negative.
- The status word: the program writes it once (0, before the create) and only reads it afterwards: at start-up
  it waits while it is negative, at shut-down (fn 0x43ae840) while it is positive. So a device that never writes
  it does not make the program wait. This corrects docs/console-libs/acp-xapu.md section 3.1, which inferred that
  a success without the console driver would hang here for ever. What the word means is still not known. The
  event at 0x9ae98c0 is only used at shut-down.
- The 64-byte records are filled by the program's per-packet feed (fn 0x785d0, fn 0x78120) with packed bit fields
  (for example bits 20-23 of the dword at +4 set to 4, bits 22-26 of the dword at +0 set to 0x17, the top bit of
  byte +7 set) and 32-bit APU addresses of the record's two buffers at +0x1c and +0x20. They have the form of a
  hardware descriptor for the console's audio processor, not of a library call. Their meaning is not in the
  executable.
- No codec name (Opus, XMA, AAC) appears in the executable's text.
- Also corrected: the table in section 6 of acp-xapu.md is older than the weave: AcpHalAllocateShapeContexts,
  ApuCreateHeap, ApuAlloc and the address functions are answered with real memory now (weave/acp_hal.cpp).- Follow-up (same day): fn 0x55cbd10, called by the per-packet feed (fn 0x785d0) with a buffer and its size
  before the records are filled, walks the data in packets of 2048 bytes. Per packet it requires the low 3 bits
  of byte +2 to be 1, takes the top 6 bits of byte +0 times 512 as a sample count, and steps
  (byte +3 + 1) x 2048 bytes to the next packet. Every one of these fields is where the public XMA2 packet
  header has it (frame count 6 bits, 512 samples per frame, packet metadata 3 bits = 1, packet skip count), so
  the data this path hands the audio processor is XMA2 (measured against the public format). ExCorvus knows the
  audio library to be Opus; the stream channel (xapu, opcode 3) submits blocks of 960, which is 20 ms of Opus
  at 48 kHz - not measured yet. So both formats may be in use (her view: music and voice lines may differ).
- Downloaded for that, outside the repository (rule 8), source only, nothing built yet: libopus (xiph.org,
  BSD, E:\third_party\opus at fed7c79) and FFmpeg, whose libavcodec has the public XMA1/XMA2 decoder (LGPL,
  E:\third_party\ffmpeg at 420f158).- Follow-up: the stream channel's buffer submit (fn 0x4f20ad0, opcode 3, called from a queue of pending submits
  in fn 0x4f20990). Per stream index the program has an input slot of 640 bytes at 0x9ae98a8 + index x 640 and an
  output area of 0x1e00 bytes at 0x9ae98a8 + 0x14000 + index x 0x1e00. It copies the packet without its first
  two bytes into the input slot and hands over: +0x24 = 0x3c0 (960), +0x28 = packet bytes, +0x2c = the input
  slot, +0x34 = channels (byte [obj+0x24]) x 0xf00, +0x38 = the output area, +0x1c = a float from [obj+0x18],
  +0x20 = [obj+0x20]. 0xf00 = 960 x 4 bytes, so the output asked for is 960 samples of 4 bytes per channel,
  up to two channels (0x1e00) - decoded sound written back into the program's own memory. So this channel is a
  decoding service whose result comes back to the program (measured layout; that the 4-byte samples are floats
  and that [obj+0x18] is the sample rate is inferred). 960 samples per packet is Opus's 20 ms at 48 kHz;
  that the packets are Opus is still not shown from the data itself. The channel is not reached in today's runs:
  XApuConnect comes after the AcpHal setup, which stops first.- Follow-up: the first step, the sound device's object (docs/console-libs/acp-xapu.md 3.1, re-read against the
  code). The program calls only four of its slots: [0] start (0x1000, 0x1000; negative = audio start-up fails),
  [0x10] the command door, [8] and [0x28] on the way down (results ignored). Of the commands only opcode 2 (setup,
  0xf0f0f0f0, block {0xc0}) has its result used (>= 0 = success). Opcode 0x12 (0xf1f1f1f1) is sent by fn 0x78120
  after it copied new data into one record's ring of 0x1700 bytes (the position at [record+0x30] wraps at 0x1700);
  its block is {voice, bytes >> 8}: "this many 256-byte units of new data for this voice". Opcodes 5 and 7 carry
  a stream id. The records' memory pointer (0x9ae9908) is used only by fn 0x78120 and fn 0x785d0 (and set up by
  fn 0x43aec40). Where the decoded sound goes - played out by the device itself, or read back by the program -
  is not read yet.- Follow-up: what the device gives back, read in fn 0x785d0 and fn 0x78120 (the only functions besides the setup
  that load the records' pointer 0x9ae9908). fn 0x785d0 starts a voice: it zeroes its 64-byte record and writes
  it field by field: a rate class in bits 27-28 of dword +4 by thresholds 24000 / 32000 / 44100 Hz, a channel
  bit 29, the ring size 0x17 in bits 22-26 of dword +0 (0x17 x 256 = 0x1700, the ring's size), a 4 in bits
  20-23 of dword +4, values from the stream's header into dwords +0xc and +0x10 and bits 12-13 / 17-19 of
  dword +4, a valid bit (top bit of byte +7), and the device addresses of the ring and of a second 0x200-byte
  area at +0x1c / +0x20. Before that it walks the stream's XMA2 packets (fn 0x55cbd10). fn 0x78120 then reads
  back two 5-bit positions in 256-byte units: bits 27-31 of dword +0 and the low 5 bits of dword +0x24. Neither
  is written anywhere in these functions (the record is zeroed at the start), so both are written by the device
  (measured: no write by the program in the functions that hold the pointer; that nothing else reaches the
  records is not shown). From their difference the program works out how much of the ring the device has used,
  refills that much and sends opcode 0x12. So the device must keep these positions current as it consumes the
  data. Where the decoded sound itself goes is still not read: nothing in these two functions reads decoded
  samples back (the 0x200-byte area is only copied between two records of the same stream).- Follow-up: where the stream channel's decoded sound goes (fn 0x4e64b10, the only other reader of the payload
  area 0x9ae98a8). Per stream it takes the count the device reported (the stream's state entry +0x334, in
  bytes, /4 = samples), reads that many 4-byte values from the stream's output area (0x9ae98a8 + 0x14010 +
  index x 0x1e00), multiplies each by 32767.0 and packs them to signed 16-bit (vcvtps2dq + vpackssdw) - so the
  device's output is float samples in -1..1 (measured: the constant 32767.0 at .rdata 0x615b954). The 16-bit
  samples go through fn 0x4e64f00 into a ring inside the program's own object (+0x100), then the stream's state
  is set to 4 (fn 0x4eaa940). fn 0x4e64f00 is the program's ring copy (a direction flag: 0 = into the ring, 1 =
  out of it); fn 0x78120 uses it with flag 1 to copy from a ring in its own object (+0x10) into a sound-device
  voice's 0x1700-byte input ring, and counts what it copied as bytes / 2 / channels ([obj+0x2004]) - 16-bit
  samples. So the sound device's voices are also fed 16-bit samples, not only XMA2 (measured for the counting;
  that the stream channel's ring and the voice's source ring are the same ring is NOT shown - the two objects'
  layouts differ). Picture so far: the stream channel decodes (960 samples per packet) and hands float samples
  back; the program turns them into 16-bit samples and gives them to the sound device's voices, whose mixing
  and output are the device's (not read). Nothing in the program reads mixed sound back.- Follow-up: the remaining commands. Opcode 5 (fn 0x43a73e0, magic 0xf6f6f6f6, block {voice}) is sent by fn
  0x78120 when a voice has not started, and the voice's table entry +0x10 is then set to 2: start a voice.
  Opcode 7 (fn 0x925e0, r8d = voice + 1, block {voice}; called by fn 0x78120, fn 0x92580, fn 0x50f2ae0) sets
  entry +0x10 to 0 and +0x18 to 1 (the value the setup gives a free voice): stop and free a voice. Opcode 2
  (setup) carries one constant, 0xc0; what it means is not readable from the program's side. The command door's
  form is the same everywhere: (object, opcode, magic or value, -1, pointer to a small block, 0). Nothing the
  program sends (commands, the 64-byte records, the setup) carries a volume, a mix or an output channel layout -
  in what was read the program gives the device no mixing instructions. fn 0x785d0 also writes device
  addresses of two more buffers into record +0x14 / +0x18 on one of its paths; what they are is not read.- Follow-up: the two extra buffers. On the XMA2 path fn 0x785d0 writes into record +0x14 and +0x18 the device
  address of the stream's own packet data (the pointer it walked with fn 0x55cbd10, after skipped packets; each
  required to be 2048-byte aligned), and the data's length in 2048-byte packets into the low 12 bits of dword +0
  (bytes >> 11, at most 0x7ff800 bytes). So these are the decoder's input, pointing straight at the program's
  packets, not an output area. Nothing read so far gives the program decoded XMA2 samples back.
- ExCorvus, on where the volume is (2026-11-27): "the volume is in the web"; "it's a weave because it is an
  active process while the game runs, but the programm itself is a web". The program is the web; the weave is
  the running process through it. So the volume is to be looked for inside the program, not added by the weave.
## 2026-11-27 - the measurements made repeatable: measure/ (first script: audio)

ExCorvus (2026-11-27): "we need to commit it in a way that people can repeat the step to here and independently
verify"; "basically if they have the game, they should be able to help". Until now most readings of the
executable were made with tools outside the repository (one of them a private local map that is never shared),
so nobody else could repeat them.

- New folder `measure/` (our own code; not part of the build or the game test, so the commit gate for weave/ and
  scripts/ does not apply - ExCorvus's choice): `pe_image.py` reads the executable named on the command line,
  read-only and in memory (sections, functions from its own `.pdata`, direct callers, rip-relative references,
  the number or offset held by one instruction); `audio.py` checks the statements of today's audio entries and
  prints `same` / `DIFFERS` for each. Only addresses and numbers are kept in the scripts, no instruction text.
- `python measure/audio.py <executable>` on this copy: 41 of 41 the same.
- Correction found by it: the payload area 0x9ae98a8 is used by four functions, not three. The fourth, fn
  0x5a87480, is the start-up initialisation of that data and only writes it (zero); the earlier reading had
  dismissed the hit as a false one because the tool used then decoded it wrongly. The statement "fn 0x4e64b10 is
  the only other reader" still holds.- Follow-up: measure/topology.py repeats the executable's part of the three topology entries (12 of 12 the same).
  Correction found by it: the entry above says "211 instructions in the executable read or write" the field
  +0x1237d. Counted again: 430 instructions address it; 211 of them store a byte into it, 189 of those a fixed
  number. The numbers of places writing 0x11 (39) and 0x13 (2), and of functions setting them (37, 33 of them
  setting nothing else), stay as they were.- Follow-up: measure/builder_shaders.py (with the shared bitcode reader measure/dxil_bitcode.py) repeats the
  builder library entries: 52 shaders, their nine parts, the root signature flags 0x3000000 in all, feature flags,
  stage and thread groups, insertvalue in five shaders, the 33 console shader compiler functions (9 shaders use
  none), the 32-lane switch in 42, the compiler named in all. 13 of 13 the same. The functions' operation numbers
  are not repeated by a script yet.
## 2026-11-27 - the builder's pipelines made repeatable: measure/builder_pipelines.cpp (measured)

- `measure/builder_pipelines.cpp` repeats the entry "the builder's 52 shaders made with their own root
  signatures" outside the program, on anyone's own copy: it reads XboxBvhBuilder.dll read-only into memory and
  makes all 52 compute pipelines on one device of the vkd3d-proton fork, each with the root signature from the
  shader's own RTS0 part, in the builder's order measured in game test 01:13. Nothing is dispatched.
- Run 2026-11-27 with the fork's libraries and the Vulkan validation layer: 9 of 9 the same - 52 containers, 49
  of them found in the measured order, 52 root signatures, 52 pipelines made, none refused, the bits 0x3000000
  noted and taken, no error line of the PC library, the validation layer loaded and silent.
- The same program beside an unchanged vkd3d-proton (71cae09): 9 pipelines made, 43 refused, 81 error lines. The
  9 made are exactly the 9 shaders that use none of the console shader compiler's functions (compared by size
  with the per-shader list). So the check can tell the fork's work from its absence.
- The earlier checker that passed shaders through the translator alone is not part of measure/: it gave every
  buffer to the shaders by GPU address, which is how the false "52 of 52 accepted" came about (entry above).
  This program takes the path the PC library itself takes.
## 2026-11-27 - two more results made repeatable: the imports and the watchdog (measured)

- `measure/imports.py` repeats the file part of "the installation's own libraries" (2026-11-26): 10 of 10 the
  same - 68 libraries and 587 functions imported by the executable; the shipped XboxBvhBuilder, Bink2Scarlett and
  FidelityFX upscaler ask exactly the console functions named there; XAudio2_9.dll is Windows' own; nothing the
  shipped libraries ask for is missing. One wording made exact: the entry says "six" shipped libraries and names
  seven files (VCRUNTIME140 and VCRUNTIME140_1 written as one); the script counts seven.
- `measure/watchdog.py` repeats what "the program's progress reports" (2026-11-26) stands on: the watchdog as
  docs/exe-map/MAP.md section 4 describes it. 26 of 26 the same after the first run, which showed five
  differences. Each was read before anything was changed:
  - three were the script's own blind spots, not the map's: the crash-callback installer fn 0x22161f0 is a leaf
    with no `.pdata` entry, so a scan over `.pdata` functions cannot see its store (read directly now, at
    0x221620c); the countdown of 30 passes is kept in a register (9 places set it), not stored in memory; and of
    the 23 functions that run the loop fn 0x1c82b20, 6 reach it by a tail jump, not a call (20 calls, 6 jumps).
  - two were the map's: the progress stamp 0x71b6338 is written by the program's reporter fn 0x22159f0 and also
    by the watchdog itself, 4 times, when it re-arms (the table said "only"); and the reporter is 35 bytes, not
    18 (what it does - one QueryPerformanceCounter, one store - was right). MAP.md corrected in both places.
- The shared reader `measure/pe_image.py` now reads import tables (`imports`, `import_at`) and finds functions
  that call or tail-jump to a target (`callers_and_jumps`); its reference scan states that it sees only code
  covered by `.pdata`. audio.py and topology.py still give 41 of 41 and 12 of 12 with it.
## 2026-11-27 - the measurements run inside the game test; where the runs stopped, made repeatable (game test 04:06)

- ExCorvus: "we need to fold the tests into the repo" - chosen: the game test runs measure/. `scripts\game_test.ps1`
  now runs every script of measure/ on the copy before it starts the game (the C++ one compiled with the weave's
  compiler, beside the weave's PC graphics library), writes their output to measure.txt of the run, and gives
  FAIL when one says DIFFERS, or when a script that checks results names no file to read.
- The first run with it (04:01) passed but ran only five of the seven: audio.py and topology.py named their input
  in another wording, and the game test skipped them without a word. Both usage lines are now written the same
  way, and a silent skip is now a failure.
- Checked that it can fail: a temporary script made to say DIFFERS (never committed, deleted after) gave
  FAIL at 04:04 with that script named.
- New `measure/stops.py`, 16 of 16: the audio setup's breakpoint at image+0x43ad94e and the boot b169 unwind
  chain; the pipeline store fn 0x2479760 checks before device slot 86; the builder's construction at
  image+0x247944f (the installation's constructor, with false); device slot 62 at image+0x247fe66. One thing read
  that FINDINGS had not said: the breakpoint is reached four ways - after the call at 0x43ad933 (sign test), after
  the audio setup fn 0x43aec40 returns false (the way of our runs), after fn 0x5b52510 returns false (0x43ada59;
  what that function is, is not read), and by falling through after fn 0x43aee50 returns false.
- Game test 04:06 (visible, built fresh): PASS, the copy's folder unchanged, the seven measurements 41, 7, 13,
  10, 16, 12 and 26 the same; the run still ends at the audio setup's breakpoint.
## 2026-11-27 - the memory map made repeatable: measure/memory.py (measured, no run)

ExCorvus asleep; read-only work on the copy, no game or GPU run.

- New `measure/memory.py`, 37 of 37 the same: docs/exe-map/MAP.md sections 1.1-1.3, 1.6 and 1.7 (their verified
  parts). The memory start-up fn 0x1e1cf60 (1,677 bytes, one caller, run-once guard 0x71bf9f8; the value from
  fn 0x55deb88 tested against 8 and the mask 0x1c2; view size ((fn 0x75140() ^ 1) | 2) << 33;
  D3D12ConfigureVirtualMemoryX with fn 0x4da2350(ceil(size / 16 GiB)) + 2, int3 on failure; the two views with
  attributes 0x21/protect 4 and attributes 3/protect 0x801; the loop over 9 pool sizes from 0x71bf9b0 with
  0x183/0x22004 and 0x283/0x2404, ten 16-byte slots, the four allocators, the pool switch 0x674c270). The
  streaming page pool fn 0x1e6c110 (951 bytes, one caller; the view globals touched by these two functions only,
  7 references; 64 KiB pages; XMemAllocatePhysicalPages with 0x380; the pool range with attributes 0x23/protect
  0x2004 kept in 0x71bfaa8; device methods +0x250 and +0x40; the free list's end 0x1fffff). The page mapper
  (the leaf 0xf2c460 and its one calling function fn 0xe88f0, stored at .rdata 0x62fc790; the six operation
  kinds at the code places MAP.md names; lock 0x71bfad8; one XMemMapPhysicalPagesScatter; the GPU batches to queue
  method +0xf8 with the pool address). The reference counts of section 1.6 (53 in 14, 47 in 34, 59 in 41, 5 in 3,
  2 in 2) and the buddy allocator sizes and callers (495 and 199 bytes).
- No correction to the map this time. One blind spot of the script itself was found and closed on the first run
  (the test against 8 lay one instruction past the window it looked in); the map was right.
- Not repeated: what the values mean (MAP.md marks those U or I), and section 1.4/1.5, which are reasoning and
  hosting, not readings.
## 2026-11-27 - the descriptor map made repeatable: measure/descriptors.py, and a correction (measured, no run)

ExCorvus asleep; read-only work on the copy, no game or GPU run.

- New `measure/descriptors.py`, 29 of 29 the same after the correction below: docs/exe-map/DESCRIPTORS.md
  section C. The record size 0x75e7000 (slot 15 asked for heap types 0-4 in a row, only the first answer stored,
  at 0x2478e4c; the four places it multiplies an index), the sampler heap offset 0x75e7010 (one store); the five
  places where the program copies 32-byte descriptors itself, each of which loads the device, tests its byte
  +0x18 and otherwise calls device slot 24; the canned null descriptors (format 0x2a, view dimension 2, mapping
  0x1688, five made through the public view call, their addresses turned into indices); the type-0 heap's
  requested address and the int3 at 0x1cf336c; the staging sampler heap asked for without an address.
- Correction (DESCRIPTORS.md C.2 point 2, changed in the map): the three record copy functions take the place from
  bits 16..25 of the record (`bextr` control 0xa10), not bits 4..11. They do not share one null table: fn
  0x4fd2b50 falls back to five null descriptors at 0x75e7540 (index bits 12..13, or 4), fn 0x4fd2c60 to a second
  set of five at 0x75e7570, fn 0x4fd2bf0 to one descriptor at 0x75e7598 and writes to its first argument itself
  (not to its +0x10). What the second set and the single descriptor are was not read: they lead into the
  descriptor tables, which are not followed tonight.
- Not repeated: the device byte's other places found on 2026-11-26 (they were written only in a working note,
  not in FINDINGS or the map).
## 2026-11-27 - the device's creation and the builder library made repeatable: measure/graphics_calls.py, and a correction (measured, no run)

`measure/graphics_calls.py` repeats docs/exe-map/GRAPHICS-CALLS.md sections 1.2 and 4 on the copy, read-only:
27 of 27 the same after the corrections below.

- Held as written: fn 0x2478440 is the only function that asks for the device, through a jump at 0x5f3fd80 to the
  D3D12XboxCreateDevice import, after the program has registered, created and shown its own window; the 64-byte
  creation block (cleared, then 0x20004 at +0, 0x100000 at +8, two settings bytes at +0x20 and +0x24, 1 at +0x28,
  0x20 at +0x3c); the arguments (no adapter, the block, the interface id, the device arriving at 0x75df048); custom
  heap properties for heap types 1, 2, 3; one pool queue; the builder object of 0x18 bytes made with the device and
  false and kept in global 0x75df0a8; the seven builder methods and every call site the map names.
- Corrected in the map (GRAPHICS-CALLS.md 1.2): device slot 48 is called **four** times (the map said five), and
  slot 41 (command signatures) **five** times, each without a root signature (the map said one; its description
  fits the first of the five). Slot 49 is the first of these calls, made only when a settings byte is 1 and a
  version is 10.0.26100.9628 or newer. Besides global 0x8759d00 the device is copied into ten more globals.
- Not read here: what the other four command signatures describe, what the objects of slot 48 are, what slot 49
  does.
## 2026-11-27 - the command list map made repeatable: measure/command_lists.py, and corrections (measured, no run)

`measure/command_lists.py` repeats docs/exe-map/COMMAND-LIST-ORDER.md sections 1 and 2 on the copy: 26 of 26 the
same after the corrections below. It checks the list object's fields in the four flush functions (sizes 6,037,
1,005, 1,027 and 935 bytes), the record words 0xc000b600 / 0xc000b800 / 0x4001b700 (0xc000b600 occurs once in the
whole file, inside its writer), that the builder library is handed the list at 0x24902ac, and each method slot at
each of the places the map names (Close, CopyBufferRegion, ResourceBarrier, ExecuteIndirect, the 40 "emit this
record" places, and the four console-only slots).

Corrections written into the map:
- Byte `+0x140` is not only set after a test that the record's first word is below 0x40000000. That test comes
  before 15 of the sets (5 in each of fn 0x32130, 0x248c2a0, 0x248c6b0). Four sets have no test: after the 28-byte
  record that also copies bit 20 into `+0x141` (one in each of fn 0x32520, 0x248c2a0, 0x248c6b0) and after a run
  of records in fn 0x32520. Two of the places the map listed for `+0x140` (0x32940, 0x248c44c) are the branch and
  the record store next to such a set, not the set itself.
- One place listed for `+0x18` (0x32a48) reads a renderer field instead.
- The two 16-byte blocks `+0x544` / `+0x554` are also written in fn 0x248c6b0 (0x248c705, 0x248c712), not only in
  fn 0x248c2a0 and fn 0x32520.
- Close (`+0x48`): its result is tested after all three calls, not at one place only. In the graphics set-up a
  negative result stops the program (int3); at the third place it returns.

What the slots mean on the PC is not checked by the script; the map still marks most of it unknown.
## 2026-11-27 - the console libraries' call sites made repeatable: measure/console_libs.py, and two corrections (measured, no run)

measure/console_libs.py repeats what docs/console-libs/ says about where the program asks chad.dll, XCRDAPI.dll,
xg_xs.dll, the three network libraries and the run-time loaded xgameruntime.dll (the sound libraries are in
measure/audio.py). On this copy: 53 of 53 the same, after these readings:

- **XDspActivate is not dead code** (chad.md section 2 said its wrapper has no referrer at all). The wrapper is
  reached by a jump at 0x11d4740 that ends a small function at 0x11d4720 with no `.pdata` entry, which is why a
  search over the `.pdata` functions did not see it. That function is called at 0x3b97e (fn 0x3b770) and 0x43b3966
  (fn 0x43b3920). fn 0x3b770 hands the address of the stream's +0x70, uses the index at +0xac and keeps the result
  at +0x60 - the fields XDspDeactivate's caller reads (section 3). Whether a run reaches these two functions is not
  measured; what the arguments mean is not read. chad.md corrected; its proposal for this function is left as it
  was, for ExCorvus to decide.
- **The storage factory's interface id has a wrong digit in the map and in the weave**: the program asks for
  `{62959d11-d45f-4884-...}` (bytes `84 48` at 0x62fc7de; the weave's own log of the request in game test 04:06
  shows `-4884-`). xcrdapi-dstorage.md said `-4888-` and corrected. weave/xcrdapi.cpp, weave/xcrdapi.h and
  weave/tests/test_xcrdapi.cpp carry the same wrong digit: the weave answers DStorageGetFactory whatever the id, so
  the runs were not stopped by it, but its log line never adds "the console factory IID the image asks for", and
  its factory's QueryInterface would not recognise the program's id. Not changed tonight (no weave changes).
- Smaller: XDspDeactivate's chain has one more jump than chad.md section 3 listed (0x1166020 jumps to the import
  thunk 0x5f3ff10); added.

Everything else held as written: the five chad imports and their thunks, XDspConnect's chain from the audio start
fn 0x43ad8b0 (whose order CoInitializeEx, ApuCreateHeap, fn 0x43aec40, fn 0x43aee50, fn 0x5b52510 is repeated),
the six XDspSubmitCommand sites, XDspDisconnect from the teardown; DStorageGetFactory's one site, its arguments,
int3, the 32 MiB call and the guard's six callers; the texture library's four plus two sites, their argument shape
and callers; the network import counts, WSAStartup's single site with its int3 and its start-up chain, the socket,
bind, listen, accept and connect sites; and the runtime loader, its six cached exports, the query helper's 161
callers and the 58 wrappers that poke the runtime first.
## 2026-11-27 - the maps checked with Ghidra: four "no caller" claims were wrong, and why (measured, no run)

ExCorvus asked for the executable to be read with REA (Ghidra 12.2, the copy's file read-only) to see whether the
agents who wrote the maps made mistakes. Every claim in docs/ of the kind "no caller at all", "dead", "one
caller", "only reader", "only store" was checked against Ghidra's references, which also see small pieces of code
that have no `.pdata` entry and function addresses kept as data.

**The mistake, and its one cause:** every claim that did not hold was a reachability claim made from a search that
saw only the functions listed in the executable's `.pdata` table, or from what one run showed. The executable has
small pieces of code without a `.pdata` entry - a null check and a tail jump, or an argument shuffle and a tail
jump - and every caller behind one of them was invisible to that search. measure/pe_image.py had the same blind
spot; it now has `reached_by`, which follows such pieces, and measure/console_libs.py checks each case (61 of 61
the same on this copy; every other measurement re-run unchanged).

Corrected in the maps:
- xgameruntime.md: the three runtime methods it called "no caller at all in this build" (#2 `+0x30`, #5 `+0x70`,
  #15 `+0x138`) are all reached, each through such a piece: `+0x30` from ten functions through 0x2213b10, `+0x70`
  from fn 0x3e40e80 through 0x3e41b40, `+0x138` through 0x2937bf0, whose address fn 0x1ce6f10 keeps for a later
  call. Of the 161 runtime wrappers these three are exactly the ones a `.pdata`-only search cannot reach.
- xgameruntime.md section 0: the method that hands out the sandbox text (fn 0x55deaf4) has four callers, not one.
  Only fn 0x3dda560 compares the text with RETAIL and the others; what the other three do is not read.
- network.md 2.1: the one WSAStartup assertion has a second way in (through 0x5a61130 from fn 0x221edc0, which the
  runtime set-up fn 0x1d109b0 calls).
- (Found the night before by measure/console_libs.py, the same cause: chad.md's "XDspActivate is dead code".)

Held as written: every "only store / only reader" claim about a global that was checked - the descriptor size
global 0x75e7000 (with three more readers not listed in DESCRIPTORS.md C.1), 0x75e7010, 0x75df188, the sound status
word 0x9ae98c8, the audio areas 0x9ae98a8 and 0x9ae9908, the watchdog's progress stamp 0x71b6338 and crash callback
0x71b6348, the two null descriptor sets, the frame-list table 0x75e72a0..0x75e7318, the chad counter 0x9aea514 -
and the "one caller" claims of MAP.md 1.1, 1.2 and 1.6.

Nothing in the weave was changed. Whether any of the newly reachable places is reached in a run is not measured.
## 2026-11-27 - the frame rhythm and the frame pacer, read in the executable; the weave's own pacer removed (game test 12:42, measured)

- The program sets its frame rate itself. One object of the program, the frame pacer (named by what it does; no
  class name is in the executable), takes a target frame rate, turns it into an interval of 1,000,000 / fps
  microseconds (fn 0x1e0fb30, kept at 0x98f84ac) and, through fn 0x1d70c40 -> fn 0x22434a0, also sets how many frames
  back the program waits (0xa94390c). Its methods sit side by side in one method table (.rdata near 0x6194d58).
  The graphics set-up (fn 0x2478440) hands that interval to the console clock: SetFrameIntervalX with 33,333
  microseconds (30 a second) through fn 0x247ab10, which three more places call when the clock changes.
- Each frame (fn 0x2481e90, reached through a table) the program asks for the current frame start (WaitFrameEventX
  event 0, no wait), keeps the frame token in its own timing table (0x75de4b0, 16 entries), waits without a time
  limit for event 2 of the frame it names from that table, and asks the frame statistics (device slot 81, in
  Microsoft's public samples GetFrameStatisticsX). Public samples (Xbox-GDK-Samples, FramePacing) give the names and
  the argument order of these four calls; the numbers of the event types and flags are not public.
- A frame ledger (every call of slots 78 to 81 with time, thread, return place, token in and out, time held; first
  1,500 calls) showed in run 12:19: the program passed its frame loop about every 1.1 ms (about 900 times a second);
  991 of 997 waits were answered at once, because on the weave's frame clock the frame a token named was always
  already past (the weave read token t as frame t - 1, a rule of its own). Slot 81 is asked on every pass and is not
  answered.
- Changed in weave/d3d12_xs.cpp: the present pacer of the weave's own (one picture per interval, cut C5) is
  removed - the program keeps its own timing; the weave no longer shows the program's window when the program has
  left it hidden; a wait for token t now lasts until frame t has come on the clock; the frame ledger and a logged
  answer for slot 81 (still not implemented) were added. weave/xcrdapi.*: the console storage factory IID is
  {62959d11-d45f-4884-...} as the executable holds it (the map had -4888-).
- Game test 12:42 (visible, Windows spatial sound switched on before the run): PASS - alive at the end, the copy's
  folder unchanged, every measurement the same. The watchdog's progress stamp was renewed 5 times (runs before: once),
  the longest it stood still was 1.5 s (before: about 6 s; once 96 s). The frame calls fell to 292 in 60 s; waits are
  held up to 30.5 ms. The sound device stop (int3 at image+0x43ad94e after AcpHalCreate) is still logged.
- What this is: an observation. The fast frame loop was a symptom of parts of the web that are missing (ExCorvus:
  "it clocked 900 times because it was missing important parts of the web"); the run does not show that the wait
  rule was the cause. Still missing: the frame statistics, and the sound device.
- Also measured today, read-only: the program creates Windows' own spatial audio stream itself (ISpatialAudioClient
  in fn 0x1e7f580, set up from the audio engine start); both console sound chips only decode, and the program copies
  the decoded samples out and mixes itself. The weave must not make a sound output of its own.- The watchdog is the weaver (ExCorvus). It does not pace the frames or run any of the work itself. It checks that
  the threads of the one program keep moving together: two threads renew its progress stamp (the run loop fn
  0x1c82b20, and fn 0x120790 with fn 0xc9e350, reached through a table), and when the stamp stands still for about
  30 seconds the program stops itself. Its progress reports are therefore the measure of whether the web holds; the
  frame pacer, the timing table and the frame statistics are threads it watches, not parts of it.
## 2026-11-27 - which files the program asks Windows for (game test 13:37, file trace on, measured)

The weave's file trace (`WHIWHI_FILE_TRACE=1`) in a visible game test; the run ended at the sound device's stop
as before (image+0x43ad94e).
- 26 file calls. The program checks and opens **12 of the installation's 22 archives** through its package drive
  G:\ - appdata0, common, movies, shaders, anim, data, levels, navmeshes, packs, textures, audio, script - and
  every one is found. This corrects the earlier reading that only 5 were opened: that count came from the
  storage requests, not from the files the program opens.
- One file beside the program: `xgameruntime.dll` in the work folder (found: the weave's own). Nothing else is
  looked for next to the program; so far "all files together" is not what stops it.
- One file is not found: `t:\telemetrycache.dat`, error 3 (path not found). The program names a second drive, T:,
  which the weave does not answer. What the program does with that failure, and what T: is on the console, is not
  read yet.
- The window pictures did not work in this run (the tool found no visible window although the log says the
  program showed it); not followed.
## 2026-11-27 - the T: drive: what the program keeps there (measured, no run)

Follow-up to the file trace of 13:37, read in the executable and in Microsoft's public GDK documentation.
- The program names T: in four places of its own code: fn 0x1dec9c0 builds `t:\` + `telemetrycache.dat` (format
  `%s%s`), opens it through its own file layer and reads up to 0x200000 bytes into a buffer; when the file is not
  there it notes the failure with an error code and carries on (it is not what stops a run). fn 0x58d7840 hands out
  `t:\` as a root path. fn 0x2213d00 and fn 0x2214540 put a wide `t:\` in front of a name and write the file
  ("WER - Saving %ls - %I64u bytes": crash reports). fn 0x308240 builds `T:\` + `%s/studiocache/%s/%d`, next to a
  `D:\` string.
- Microsoft's public page "Local storage on Xbox console" names the console's drives: G:\ installed game data
  (read-only), **T:\ temporary local storage** (2 GB, written by the title while it runs, content deleted after the
  title ends, kept across suspend), D:\ developer scratch partition (development only). Persistent local storage
  has no drive letter; the title asks for its path (XPersistentLocalStorageGetPath). The installation's
  MicrosoftGame.config asks for 2048 MB of persistent local storage.
- So T: is not the installation: it is a writable place of the program's own that starts empty. That the telemetry
  cache is missing at the start is what a console shows too.
- What the weave answers today: G:\ only. A call for T:\ goes to Windows as written and fails (error 3).
Follow-up, game test 13:57 (PASS: alive at the end, 5 progress reports, frame token 1,637; the run still ends at
the sound device's stop), with T:\ answered for this one run from a temporary folder (that change was taken out
again afterwards and never committed; T: is now meant to be a real drive, docs/BUILD-AND-RUN.md):
- Correction: the program does not only read the telemetry cache. With T: there, its `CreateFileW` for
  `t:\telemetrycache.dat` succeeds and the file is left behind at exactly 2,097,152 bytes (0x200000, the size
  fn 0x1dec9c0 hands to its file layer): the program makes the file itself, at its full size. Its contents were
  not read.
- Nothing else was written to T: in that run.
## 2026-11-27 - the sound chip's two ring positions: which is which (measured, no run)

Read in fn 0x78120, the program's read-out of a voice's ring (it takes the chip's decoded 16-bit samples out of the
ring into its own, fn 0x4e64f00 with flag 1, and then sends opcode 0x12 with the number of bytes taken):
- **Decoded up to** = bits 27-31 of record +0, in 256-byte units (shr 0x13, and 0x1f00 at 0x78214/0x7821b).
- **Taken out up to** = low 5 bits of record +0x24, in 256-byte units (0x78205, 0x78211, 0x78218).
- Ring size = bits 22-26 of record +0 (0x7822e/0x78231; the program writes 0x17 there = 0x1700 bytes).
- Available = decoded - taken; when decoded is below taken the ring has wrapped: size - taken + decoded. When the
  two are equal, the sign of the dword at record +4 (its top bit, which the program sets at voice start) decides:
  set -> nothing to take; clear -> the whole ring is full (0x782e6 / 0x782eb).
- The program never writes either position (earlier reading, measure/audio.py). So both are the chip's: it moves
  "decoded up to" as it decodes, and "taken out up to" after an opcode 0x12 has told it how many bytes the program
  took (inferred from the order of the calls); and it clears the top bit of +4 when its decoding has filled the
  ring up to the taken-out position (inferred: the only way the "full" case can arise).
- measure/audio.py checks the four new points (45 of 45 the same).
## 2026-11-27 - what the voice start hands the sound chip (measured, no run; measure/audio.py 56 of 56)

Read in the executable at the program's voice start (fn 0x785d0), so that the weave's sound chip can take exactly
what the program gives it. Everything below is repeated by measure/audio.py.

- The voice's data must start on a 2048-byte boundary (the program tests the low 11 bits and stops the voice
  otherwise). The data is XMA2 packets of 2048 bytes (the packet walk fn 0x55cbd10, entry of 2026-11-27 on audio).
- The record of the voice gets: the rate class in bits 27-28 of +4 (24,000 Hz or less, 32,000 or less, 44,100 or less,
  above that), one or two channels in bit 29 of +4 (from the object's channel count), the ring size 0x17 (0x1700
  bytes) in bits 22-26 of +0, the ring's and the second area's chip addresses at +0x1c and +0x20, the data address at
  +0x14 and +0x18, the length in 2048-byte packets in the low 12 bits of +0, the bit position of the first frame to
  play in the low 26 bits of +8, and a loop count in bits 12-19 of +0 (255 when the program's loop count is over 4).
- Where a loop begins and ends is found by fn 0x43a7440 and written into the low 26 bits of +0xc and +0x10; the
  samples are counted by fn 0x55cbd70. Neither function was read further.
- Nothing about volume, mixing or the output is in the record (as before).

What this means for decoding (not built yet): each voice is one XMA2 stream of one or two channels at one of four
rates. FFmpeg's XMA2 decoder (libavcodec/wmaprodec.c, xma_decode_init) takes exactly that: a stream count, the
channel count, the sample rate and 2048-byte packets. Starting at a bit position inside a packet and the loop
points are open: FFmpeg decodes whole packets.
## 2026-11-27 - the sound chip answers; the next stop is the stream channel (game test 14:46, measured)

The weave now hands out a sound chip (weave/acp_chip.cpp). It answers what the program measurably gives it (entries
above and measure/audio.py): method 0 starts it, opcode 2 sets it up, opcode 5 starts a voice (decoding that voice's
XMA2 packets through FFmpeg's XMA2 decoder into the voice's ring and moving "decoded up to"), opcode 7 stops one,
opcode 0x12 moves "taken out up to". FFmpeg is built separately by scripts/setup_ffmpeg.ps1 (only the XMA1/XMA2
decoders) and loaded at run time. Read in the executable before it was built: the voice number in opcodes 5 and 7 is
the record number (fn 0x43a73e0 and fn 0x925e0 use the same index for the voice table), and "start voice" is sent once
per voice (fn 0x78120 sends it only when the voice table's +0x34 is 0 and then sets it to 2; stopping sets it to 0).

Game test 14:46 (run-20261127-144642, visible, PASS: alive after 60 s, copy's folder unchanged, all 12 measurement
scripts the same):
- The program created the chip (AcpHalCreate), started it (method 0 with 4096 and 4096) and set it up (opcode 2,
  value 0xf0f0f0f0, block word 0xc0); each answer was taken.
- It then asked the second half of the sound system, the stream channel: XApuConnect (xapu.dll), still refused by the
  weave ("no audio command channel exists on a PC"). The program stopped itself at the same int3 as before
  (image+0x43ad94e), now reached the fourth of the four ways measure/stops.py lists: after the stream channel's
  connect (fn 0x43aee50).
- No voice was started yet (no opcode 5), so the decoding itself has not run on the program's data.
- New in this run: Bink (BinkSetSoundSystem, BinkStartAsyncThread, BinkSetMemory), CoCreateInstance, WriteFile and
  BCryptGenRandom were called for the first time, and 180 imports in all (153-177 before).

Not measured, the weave's choices (each logged once when used): "taken out up to" is moved by the amount the program
names in opcode 0x12 (inferred: the program never writes it); the ring is marked empty again when both positions
meet after a take; a voice starts and loops at whole 2048-byte packets, because FFmpeg decodes whole packets.
## 2026-11-27 - the game test judges the whole: the wants ledger, and the first picture of the whole (game test 15:42, measured)

- The game test is the real scenario: the whole installation runs as it is started, and the verdict no longer
  hangs on one error. Two rules stay hard (the copy's folder unchanged; every script in measure/ still holds).
  Beyond them a run is judged against the baseline's whole: every meeting point between the program and the weave
  must be the same or better, and every import provided in the baseline still provided (scripts/game_test.ps1).
- The meeting points come from a new record the weave writes while the program runs, the wants ledger
  (weave/d3d12_xs.cpp, WHIWHI_WANTS_LEDGER; the game test sets it): one line per step of the program's flow, what
  the program asked for there, and what the weave gave, with a count, the time of the first one and the place in
  the program it came from. Numbers and words only. The game test folds it into whole.txt: each point attached,
  partly or missing.
- Why "alive at 60 s" is no longer a rule (measured): every run since 11:00 reaches the program's breakpoint at
  image+0x43ad94e exactly once, and Windows' application log has a crash entry for that breakpoint for every one
  of them, including the runs that counted as alive. The program's own handler for unhandled errors
  (SetUnhandledExceptionFilter, imported by the executable) hands it to Windows, which ends the process; other
  threads go on presenting frames meanwhile. Whether that happens before or after the 60 s mark is Windows'
  timing, not a second cause. The breakpoint itself is reached because the weave's stream channel (xapu.dll)
  answers XApuConnect "not provided": the stop points to the sound device, but its cause is in the weave.
- Run 15:42 (run-20261127-154207, visible): copy's folder unchanged, 13 measurements the same, PASS. The whole of
  the graphics library: 19 points attached (all six kinds of descriptor heap, every kind of view, the sampler
  copies, the builder's 53 compute pipelines, triangle topology, pipelines, root signatures and tables set on the
  lists), 1 partly (the program's graphics pipelines: 2,905 made, 47,564 not made), 19 missing (the program's own
  compute shaders, slot 62, and their lookup, slot 67, 2,689 each; base pipelines, slot 57, 25,450; samplers,
  slot 61, 43; the rectangle topology and all 54 draws; 13 command-list methods).
- A mistake of the gate's own, found in its first run: it counted the "not provided" points as attached (35
  attached, 3 missing). Its pattern looked only at the answer's words. Corrected and counted again from the same
  ledger.
- Every pipeline the weave's translator refuses is a C++ error the weave throws and catches itself on the program's
  worker threads: about 47,000 in about 8 s of this run. They do not reach the program; what they cost in time on
  those threads is not measured.
- Not in the ledger yet: the weave's other libraries (sound chip, stream channel, memory, files, runtime). They
  appear only through the import list so far.
## 2026-11-27 - the runtime's start and base object answered by Windows' own runtime, and the port installer (game tests 16:17, 16:31, measured)

- The first errors the program meets in a run are six runtime interfaces the weave refused. Followed along the
  thread: the first of them is the runtime's base object, which 58 of the program's 161 runtime calls touch before
  anything else (docs/console-libs/xgameruntime.md section 3.2). Windows has the runtime itself
  (System32\xgameruntime.dll), and the weave was answering in its place.
- Measured first without the program (a small test of its own, outside the repository): Windows' runtime starts with
  the program's own start values (InitializeApiImpl(0x6336, 0x2712) = 0) and hands out the base object; its
  methods, including +0xe8, lie in Windows' library. No network endpoint opened by that process, none by the Gaming
  Services processes (IPv4 checked).
- Woven in (weave/xgameruntime.cpp): the runtime's start (InitializeApiImpl, Ex, Ex2) and the base object are
  passed to Windows' own runtime unchanged; the weave adds nothing. The other interfaces stay as they were.
- Game test 16:17: PASS; Windows answered the start (Ex2) and the base object with success. The program then went
  on to ask for two runtime interfaces it had not asked for before ({3e241977-...}, {973a344e-...}).
- Game test 16:31, with a watch of the program's process every 2 s (29 samples over its 60 s): 0 TCP connections and
  0 UDP endpoints the whole time. The weave's offline layer refused getaddrinfo and the installation's HTTP library
  as before. Not watched: Windows' runtime service, which runs in a system process of its own. A first watch during
  the 16:17 run wrote nothing (a mistake in how it was started); that run has no network record.
- Still answered by the weave in Windows' place: the sandbox name ("RETAIL") and the remaining interfaces (user,
  networking, state reporting, one-time initialisation, two more).
- scripts/install_port.ps1: installs the weave on a PC as a personal port of one's own copy. It compiles first (the
  weave with its sample tests, the vkd3d-proton fork, FFmpeg's XMA2 decoder, libopus), then puts the weave's
  programs and libraries into a port folder outside the repository and the game folder (it refuses others), without
  the libraries the installation ships, with a start script that runs the original executable through the weave.
  Nothing of the game is copied; the weaving happens when the program runs. Installed and started on this PC at
  16:10: it ran as the game test runs and ended at the same place (the stream channel). The port folder is for the
  owner's own PC and research and is not shared.
## 2026-11-27 - every runtime interface the program asks for goes to Windows' own runtime (game test 16:52, measured)

- What changed: the weave no longer answers any part of the console runtime with an object of its own. The
  interfaces measured with Windows' own xgameruntime.dll (10.0.26100.9676, System32) go to it unchanged: the base
  object, the system interface, state reporting, the one-time start, and three more pairs the program asks for
  ({03f0fe74...}, {3e241977...}, {973a344e...}). The weave's own system object, which answered "retail", is gone:
  it was a stitch. The user and networking interfaces are held (rule 3: nothing online; ExCorvus decides). Any pair
  not yet measured with Windows' runtime is refused and logged. XErrorReport stays the weave's (Windows' own may send
  a report).
- Measured beforehand with a small test program outside the game, which loads Windows' runtime directly: every one
  of these pairs answered S_OK there except the one-time start (0x80004005), and no network connection was opened.
- Game test 16:52 (visible; copy's folder unchanged; 13 measurements the same; a network watch every 2 s during the
  run saw no connection of the process): 152 imports called (181 before), 18 answers not provided (25 before).
  Inside the program's process Windows' runtime answered the base object and the system interface with S_OK, the
  pair {973a344e...} with S_OK, but state reporting, the one-time start, {03f0fe74...} and {3e241977...} with
  0x8924010d. The test program got S_OK for those. What 0x8924010d means was not found; that it depends on which
  process asks is a guess, not measured.
- The program then takes another path: XApuConnect, the name lookups, the HTTP client start, BCrypt, the C++ stream
  functions and some file writes are no longer called. The run still ends at the same breakpoint, image+0x43ad94e,
  reached through another of the four ways into it; which one is not read yet.
## 2026-11-27 - what Microsoft's own tools say about the builder's shaders (measured, no game run)

Asked for on dxil-spirv's llvm.copysign change ("Does it pass dxil.dll validator? Is there evidence Windows drivers
don't explode on that code?"). A small program, written for that question and kept on its own branch of our
dxil-spirv fork (`evidence-xbox-builder`, folder `evidence/xbox-builder/`), reads the 52 compute shaders of the
installation's XboxBvhBuilder.dll in memory and hands each one to Microsoft's tools. Nothing is written to disk.

- **Microsoft's validator** (dxil.dll 1.9 from the Windows SDK 10.0.26100): all 52 invalid. Every one is refused
  first for its target, "Unknown target triple 'dxil-ms-xb2'", and for "Named metadata 'xbox.compiler.options' is
  unknown": these are the Xbox's form of DXIL, not the PC's. Further messages: "External function 'xbox.*' is not a
  DXIL function" for each console function, "ExtractValue should only be used on dxil struct types and cmpxchg",
  "Instructions must be of an allowed type", and for the three shaders that
  declare it, "External function 'llvm.copysign.f32' is not a DXIL function". So the maintainer's reading holds:
  llvm.copysign is not valid PC DXIL. It comes only in the Xbox's form.
- **Windows' own D3D12** (system d3d12.dll, AMD Radeon RX 9070 XT, driver 32.0.32029.2010): with the containers'
  own root signatures, all 52 refused at the root signature (0x80004005: the two Xbox-only bits 0x3000000). With
  each root signature rewritten without just those two bits (a measuring option, the shader unchanged), the first
  seven shaders gave: the two without console functions made; four with console functions an access violation
  inside AMD's D3D12 shader compiler (amdxc64.dll+0x1aab91c, the same place each time); the fifth the pipeline
  refused 0x887A0005 and the device removed with 0x887A0020 (driver internal error); no new device could be made
  after that, so the run ended there.
- **Driver and machine after it** (rule 7): the device removal stayed inside the test program; Windows' System log
  has no display or driver event from that time, and both graphics adapters report OK.
- What this does and does not show: Windows' own path does not take these shaders either; AMD's D3D12 compiler
  faults on those with console functions. Whether llvm.copysign itself would make a Windows driver fail is not
  shown: none of the three shaders that use it was reached, and all of them also use console functions.
## 2026-11-27 - one command repeats every measurement: measure/run_all.py (measured, no game run)

`python measure/run_all.py "<your game folder>" --report report.txt` runs every script in measure/ on a game folder
side by side and prints one report: the tool versions, the size and SHA-256 of each file read, and for each script
"holds", "differs" (with its DIFFERS lines) or "could not run". The report holds nothing of the game, so it can be
posted as it is; anyone with their own copy can check the results without us measuring again. The script that needs
the graphics card runs only when its built program is passed with --pipelines.

On this copy (executable 120,181,760 bytes, SHA-256 e95020b2fa4a721db3df59fdecf75621730ac559d8835252f6c3b658d5a08993;
XboxBvhBuilder.dll 884,224 bytes, SHA-256 99f803ac22c01ef329dddb1d40be67238d2a812d01e8d040b3cb5135bc23707e): all 11
measurements hold, 313 statements, in one run with the scripts side by side.- Posted on the copysign change (dxil-spirv PR 384) the same day: the measurement, the link to the evidence
  program, and the question whether dxil-spirv wants Xbox Series DXIL support as one described set (only for shaders
  with the `dxil-ms-xb2` target, so PC DXIL is not touched). Not yet shown and said so: that the builder's shaders
  compute the same results as on the console.- **Correction (same day)** to the entry "every runtime interface the program asks for goes to Windows' own runtime
  (game test 16:52)": its list of functions "no longer called" at 16:52 (XApuConnect, the name lookups,
  the HTTP client start, BCrypt, the C++ stream functions, some file writes) is wrong. The run's log shows
  XApuConnect called and refused at 16:52 exactly as before; the call ledger (calls.tsv) has 0 for it because the
  process was ended by Windows at the breakpoint before the ledger's last counts were written. So nothing shows that
  the program took another path: it stops where it stopped before, at the stream channel (XApuConnect refused, way 4
  into image+0x43ad94e). The ledger's counts are only complete for a run that ends the ledger's own way; a run
  Windows ends gives a lower bound.
## 2026-11-27 - the stream channel answers; the program passes the sound start for the first time (game test 17:41, measured)

weave/xapu.cpp now answers the stream channel the way the program was read using it (docs/console-libs/acp-xapu.md
section 5, the 17:xx entries above): XApuConnect succeeds and writes a client handle into the program's slot; open,
close and reset are answered with result types 1, 2 and 5; a submitted packet is checked with libopus and decoded
only when libopus reads it as exactly the samples the command asks for, and the result carries the bytes written
(0 when nothing was decoded). The weave's own choices are logged once (results answered at once, 48 kHz, floats
interleaved). libopus is built separately (scripts/setup_opus.ps1) and loaded from beside the library.

Game test 17:41 (run-20261127-174115, visible, the build with the change): PASS - the copy's folder unchanged, every
measurement the same, no network endpoint in 32 samples of the network watch.
- XApuConnect was answered (descriptor words 0x1, 0x100, 0x2, 0x104000, 0x0, 0x1) and the program did NOT stop at
  image+0x43ad94e, where every run since 02:57 had stopped. It went on: 192 imports called, 10 progress reports to
  the watchdog (5 before), frame token 1683.
- In the 60 s the program polled XApuGetResultCount but opened no stream and submitted no packet; the sound chip
  had no voice started. So the decoding itself is not tested yet.
- Newly called: among others AcpHalCreate, AcpHalAllocateShapeContexts, ApuCreateHeap, XMemAlloc (now counted,
  because the run is no longer ended by Windows at the breakpoint before the call ledger is written), the C++ stream
  and lock functions, BCryptCloseAlgorithmProvider, SetFilePointerEx, CloseHandle.
- New errors, not followed yet (recorded as they are): 21 storage reads failed with Windows error 38 (end of file):
  the weave's storage library (weave/xcrdapi.cpp) read past the end of the file a request names, at offsets up to
  0x277fb6000; its log does not say which file. At 57.3 s a read at address 0x733000 faulted at image+0x2942c8
  (one exception line, the run went on to 60 s). Whether the two are connected is not established. An error that
  shows up in the program may point away from its cause (ExCorvus); the failed reads are the weave's own and come
  first.
Follow-up, game test 17:57 (run-20261127-175726, visible; the storage library's failure line now names the file and
its size): PASS on the hard rules, the copy's folder unchanged. One read failed before the run ended at 54 s:
- 10,464 bytes at offset 0x10f1e6000 of audio.rpf, which is 28,543,454,672 bytes long: the piece lies INSIDE the
  file, yet the read was answered "end of file" (Windows error 38). So the failed reads are not reads past an end
  the installation has; the requests may be right and the weave's read wrong.
- The run then ended in the middle of writing that very failure line: an access violation inside the C runtime
  (ucrtbase.dll, called from the weave's XCRDAPI.dll), read at 0x7ffb00000000.
- The weave's read (weave/xcrdapi.cpp read_exactly) uses an OVERLAPPED structure on its own stack with no event of
  its own, on a file handle opened for overlapped use, and waits with GetOverlappedResult. Windows' documentation
  says that without an event the handle itself is signalled and advises against it when several operations run on
  the same handle at once: a wait can then return for another read's completion. A read that is reported failed
  but still completes later writes into memory that is no longer its own (the stack and, for zlib pieces, a freed
  buffer). That would explain both an "end of file" inside the file and faults in unrelated places (here the C
  runtime, at 17:41 the program at image+0x2942c8). This is inferred from the measured pieces and the documentation;
  that several reads ran at once on one handle is not measured.
Follow-up, game test 18:20 (run-20261127-182007, visible; the storage library now counts, per file, the reads open
at the same moment and names the reading thread): PASS on the hard rules, the copy's folder unchanged, the run
alive at 60 s (frame token 1711, 6 progress reports).
- Several reads ARE open on one file at once: two on audio.rpf (the first time at request #1, thread 40812).
- 14 reads failed, all on audio.rpf, every piece inside the file, and every one that kept its own numbers started
  while another read was open on the same file.
- The trace of the cause is in the failure lines themselves: in most of them the two numbers printed last are not
  the line's own - "0 bytes asked for at the destination" and a "thread" number that is exactly the size of the
  piece (33984, 14816, 766160, ...). Those are the two words Windows writes into an OVERLAPPED structure when a read
  completes (its status, 0, and the bytes moved). So the read reported as "end of file" did finish later and wrote
  its completion into stack memory the weave had already given to the next function - the inference of 17:57 is
  measured now.
- The fix (weave/xcrdapi.cpp read_exactly): each reading thread has its own event, every read names it, and its
  result is always taken through GetOverlappedResult, which then waits for that read alone. To be tested.
Follow-up, game test 18:32 (run-20261127-183211, visible, the build with the read fix): PASS, the copy's folder
unchanged, every measurement the same, the run alive at 60 s (frame token 1680, 9 progress reports, 196 imports
called). Two reads were again open at once on audio.rpf and on data.rpf, and NO read failed (14 before the fix,
21 at 17:41). The fix holds for this run.
- The one exception line of the run, at 57.6 s: a read at address 0x8 at image+0x161b994 (unwind +0x161b943,
  +0xeae076, +0xea8d14, +0xea8144, +0xea778b, +0x6c52a9, ...); the run went on to 60 s. Not followed yet. The
  faults of 17:41 (image+0x2942c8) and 17:57 (inside the C runtime) did not come back.
- The stream channel still opened no stream in the 60 s.
Follow-up, game test 18:49 (run-20261127-184935, visible): the weave's call counter now also keeps, per thread,
the last four imports that thread called, and a fault prints them for the faulting thread (weave/loader.cpp
set_call_history_slots, weave/tools/boot_image.cpp; the stubs use rax, which no call takes an argument in). PASS,
the copy's folder unchanged, every measurement the same. This run had NO fault at all: no exception line, no
Windows crash report (the last one is 18:43:25, the run of 18:32), alive at 60 s, frame token 1745, 64 pictures
presented, no failed read. So the fault at image+0x161b994 does not come in every run; its thread's history is
not seen yet. Seen in 3 of the last 4 runs: a fault in a different place each time (17:41 image+0x2942c8, 17:57
the C runtime, 18:32 image+0x161b994; the first two came from the weave's reads, fixed since).
## 2026-11-27 - the silent answer: chad said "success" and did nothing (game test 19:15, measured)

The program's sound circle has a fourth part besides the sound chip, the stream channel and Windows' spatial audio:
chad.dll, which takes 4096-byte pages from the program and gives back an answer page and a completion number.
ExCorvus heard that the sound stays silent and named it: a silent answer, a signal you cannot hear that confirms
the audio part of the weave.
- The weave's chad answered every call with 0 and did nothing. In the program 0 is SUCCESS at every call site; the
  map (docs/console-libs/chad.md, corrected) and the weave's own sample test had taken 0 for failure. So the program
  ran with "channel open, streams started, pages taken" while nothing answered: no handle, no status block, no
  answer page, no completion number (rule 4). It also means one of the four ways into the old sound stop at
  image+0x43ad94e was only passed because of this false success.
- Read in the exe: activate (channel, description, where the status block goes = stream +0x68, where the stream
  handle goes = stream +0x70); submit (stream, {1.0, 1.0, page in, page out, 0}, completion number out); the stream
  then waits until its status block carries the completion number (+4) and a 0 (+0xc), and takes the answer page
  in four quarters.
- weave/chad.cpp now opens the channel, gives every stream a handle and a status block, and closes each round trip
  at once. The answer page is, as a TEST of what the program expects back (ExCorvus: "test what it expects back
  first, and if that isn't the solution, it's what it wants to go in"), the input page unchanged; the first pages'
  contents are logged as numbers for the next step. The sample test that pinned 0 = failure is rewritten.
- Game test 19:15 (run-20261127-191524, visible): PASS, the copy's folder unchanged, every measurement the same,
  alive at 60 s, no fault, frame token 1755. The channel opened and its handle was written; in the 60 s the program
  activated no chad stream and submitted no page, opened no stream on the stream channel and started no voice on
  the sound chip. So the round trip itself is not tested yet: the program has not got to playing sound.
## 2026-11-27 - the presented picture is not in the program's memory; the window thread's fault at about 70 s (game tests 19:37 and 19:50, measured)

- The game test now checks that its work folder holds exactly the build it is recorded for, file by file, and runs
  nothing otherwise. Measured at 19:37: the work folder still held an older copy of d3d12_xs.dll (from 15:14),
  because removing the old files failed without stopping the script; that run used the older library. Every other
  file matched. The 19:37 run is therefore not counted for the picture measurement.
- New measurement in PresentX (weave/d3d12_xs.cpp): the exact pixel values of the picture the program presents, counted
  where it lies, with the window's focus. Run 19:50 (3 minutes asked, PASS, copy's folder unchanged, every
  measurement the same, no network): every presented picture (1, 2, 3, 4, 8, ... 128) is a texture of 3840 x 2160,
  format 28 (R8G8B8A8_UNORM), that the program asks for as a committed resource (device slot 127, heap type 1). It
  lies in the PC library's memory, not in the program's, so its values were not counted. The window never had the
  focus at any present.
- Both 3-minute runs ended at about 70 s (69 and 72 s) at the fault image+0x161b994 (a read at 0x8), and in both the
  thread that faulted was the program's own window thread (the thread of the window PresentX presents to). Its last
  imports were memcpy, __stdio_common_vsprintf, memcpy, memcpy. The 60-second runs end before 70 s, which may be why
  this fault was seen only once before (18:32). Not followed further.
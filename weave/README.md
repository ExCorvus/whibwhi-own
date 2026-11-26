# weave

The runtime, the console libraries it provides, the tools and the tests. All of it is our own code.

## The core (static library `whiwhi_weave`)

- `image.hpp` / `image.cpp` — reads a 64-bit Windows executable image: headers, sections, data directories. Can
  lay the image out as it sits in memory.
- `imports.hpp` / `imports.cpp` — lists every library the image imports and, per function, its name or ordinal
  and the slot the loader fills.
- `loader.hpp` / `loader.cpp` — places the image at its preferred address (or elsewhere, applying its
  relocations), connects each import, gives the image thread-local storage, sets page protections and registers
  its exception table. An import nothing provides gets a stub that reports the call. In its in-memory copy only,
  the loader also replaces breakpoint bytes in stubs that the image's tables point at, and a breakpoint that
  directly follows an indirect call (`nop_loader_stubs`). The loader does not start the image.
- `thread_block.h` / `tls_seat.cpp` — the 64 KiB per-thread block lent to the loaded image. It lives in the
  thread-local section of the tool's own executable, so Windows gives it storage index 0, the index the game
  expects to own.
- `host_libraries.hpp` / `host_libraries.cpp` — connects an import to the library of the same name: the weave's
  own library beside the tool first, otherwise the PC's.
- `console.hpp` / `console.cpp` — the few imports that are not answered by the same-named library: `VirtualFree`
  goes to the memory library, `GetModuleHandleA/W(NULL)` answers with the loaded game image, and file calls on
  the drive letter the console mounts the game's package as are answered from the game folder, read-only. Also
  connects the allocation routines the game exports to the memory library, and connects the game's network
  imports to the refusals in `offline.cpp`.
- `offline.hpp` / `offline.cpp` - the network boundary: a refusal for every imported function that would open a
  socket, resolve a name or make an HTTP request (documented failure value and error, one log line per function,
  a counter). Harmless helpers go to the real libraries. What each import gets is in
  `docs/console-libs/network.md`.
- `hosting.hpp` / `hosting.cpp` — address model for showing the game's memory to the GPU: one range,
  `device_address = heap_base + (game_va - flat_base)`. Not yet connected to a GPU.
- `shader_container.hpp` / `shader_container.cpp` — recognises the two headers the game's shader bytes arrive
  with; it names no instruction stream it cannot prove.
- `shader_isa.hpp` / `shader_isa.cpp` — reads the instruction words of a console shader. An instruction it cannot
  classify is reported, not guessed.
- `shader_translate.hpp` / `shader_translate.cpp` — translates a console shader to SPIR-V, kept in memory. Today
  it handles straight-line compute shaders of a handful of instructions and refuses everything else.
- `spirv.hpp` / `spirv.cpp` — our own small SPIR-V writer.

## The console libraries (one DLL each, named as the game imports them)

- `xmem.h` / `xmem.cpp` → `xmem.dll`. The console's memory library: 64 KiB physical pages, address ranges the
  pages are mapped into, committed memory. Keeps a record of every region with the flags the game asked for.
- `d3d12_xs.h` / `d3d12_xs.cpp` → `d3d12_xs.dll`. The console's graphics entry points. Creates a PC D3D12
  device and hands the game its own objects around the PC ones: functions the two share are forwarded, the
  console-only ones the game uses are written out as they are measured (queues and submission, command lists,
  fences, the frame clock, descriptor heaps, buffers over the game's own memory, textures, display surfaces,
  presentation to the game's window), and every other one logs its arguments once and answers "not
  implemented". Draws are held while no pipeline state exists.
- `xcrdapi.h` / `xcrdapi.cpp` → `XCRDAPI.dll`. The console's file-read library: our own factory, file, status
  array and queue; a read goes straight from the file into the address the game names, and a piece the game
  marks as compressed is decoded on the way.
- `inflate.hpp` / `inflate.cpp` — our own zlib decoder (written from the public format description), built into
  `XCRDAPI.dll`.
- `xgameruntime.h` / `xgameruntime.cpp` → `xgameruntime.dll`. The runtime library the game loads at start:
  initialise and uninitialise answer success, interface queries are logged and answered "no such interface".
- `acp_hal.h` / `acp_hal.cpp` → `AcpHal.dll`. Console audio hardware library: the memory functions are real, the
  audio device answers "not implemented".
- `xapu.h` / `xapu.cpp` → `xapu.dll`, `chad.h` / `chad.cpp` → `chad.dll`. Console audio command libraries:
  skeletons that log and answer failure.
- `xg_xs.h` / `xg_xs.cpp` → `xg_xs.dll`. The texture layout library: answers with the weave's own untiled layout
  for formats in the public format list, copies rows in and out, refuses everything else.
- `xbox_bvh_builder.h` / `xbox_bvh_builder.cpp` → `XboxBvhBuilder.dll`. Ray-tracing builder: a skeleton that
  logs and answers failure.

## Tools

- `list_imports <image file>` — the imported libraries and function counts.
- `describe_image <image file>` — what the image asks of a loader: base, sections, data directories,
  relocations, thread-local storage.
- `connect_imports <image file> [library folder] [--names]` — loads the image without starting it and reports
  which imports connect.
- `boot_image <image file> [library folder] [--seconds N] [--no-callbacks] [--same-thread]` — loads the image
  and starts its entry point; logs calls to imports nothing provides, and faults with a backtrace.
- `vkhost_probe` — checks that the GPU can read ordinary host memory without a copy. Built only when the Vulkan
  SDK is installed.

## Tests

`tests/` holds 17 tests (hosting, image, imports, loader, thread_storage, console, xmem, d3d12_xs, shader,
shader_container, offline, acpxapu, chad, xcrdapi, inflate, xg, gfx_stubs). They use made-up inputs only: no
game files, no GPU.

## Build and test

```
build.cmd
```

MSVC with the CMake and Ninja that come with the Visual Studio Build Tools.

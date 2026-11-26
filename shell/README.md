# shell

The runtime core.

- `image.hpp` / `image.cpp` — PE image mapper. Parses a PE64 image and maps it flat (headers at 0, each section
  at its RVA). PE structures defined from the public PE spec.
- `imports.hpp` / `imports.cpp` — import table reader. Lists every library the mapped image imports and, per
  function, its name or ordinal and the address-table slot the loader fills.
- `loader.hpp` / `loader.cpp` — image loader. Places the image at its preferred address (or elsewhere, applying
  its relocations), connects each import through a resolver, gives the image thread-local storage, sets every
  section's page protection and registers the exception table. An import nothing provides gets a stub that names
  the function if it is called. The loader does not start the image.
- `thread_block.h` / `thread_block.cpp` — `whiwhi_tls.dll`, a small library built without the C runtime whose
  thread-local section is one empty 64 KiB block. Windows gives thread-local storage only to libraries it loaded
  itself, so the loader lends this library's storage index and per-thread blocks to the image: each thread gets
  the image's initial data, and once the image is started its thread-storage callbacks are called for every
  thread that starts or ends. One loaded image can hold the block at a time.
- `host_libraries.hpp` / `host_libraries.cpp` — resolver that connects an import to the same-named library on
  the PC (or in a folder added to the search), and records which libraries do not load.
- `xmem.h` / `xmem.cpp` — `xmem.dll`, the console's memory library, answered on a PC. An image imports it by that
  file name, so it is a real library beside the shell (never in the game folder). Physical pages are 64 KiB pieces
  of pagefile-backed memory; a range for pages is a Windows placeholder reservation, and each mapped page is its
  own view, so changing one page never disturbs another. It answers 9 of the 12 functions an image imports; the
  three whose meaning is not known are left out and name themselves if called. Every region keeps the flags it
  was asked for with, graphics bits included, for the shell to read.
- `console.hpp` / `console.cpp` — where an import of a console image is connected. Everything goes to the
  same-named library except `VirtualFree`, which the memory library answers: an image frees what
  `XMemVirtualAlloc` gave it with `VirtualFree`.
- `hosting.hpp` / `hosting.cpp` — flat hosting model. One contiguous GPU backing over the game's flat VA space;
  `device_address = heap_base + (game_va - flat_base)`; real pages bound on demand.
- `tools/` — command-line tools:
  - `list_imports <image file>` prints the imported libraries and function counts.
  - `describe_image <image file>` prints what the image asks of a loader: base, sections, data directories,
    relocations, thread-local storage.
  - `connect_imports <image file> [library folder] [--names]` loads the image without starting it and reports
    where it was placed, its thread-local storage, which imports the shell's own libraries answer, which connect
    to the PC's, and which are not provided.
- `tests/` — unit tests (no game data).

## Build + test

```
build.cmd
```

MSVC + the bundled CMake/Ninja. Builds `whiwhi_shell`, `whiwhi_tls.dll`, `xmem.dll` and the tools, and runs
`test_hosting`, `test_image`, `test_imports`, `test_loader`, `test_thread_storage`, `test_console` and `test_xmem`
via CTest. No GPU needed.

# shell

The runtime core.

- `image.hpp` / `image.cpp` — PE image mapper. Parses a PE64 image and maps it flat (headers at 0, each section
  at its RVA). PE structures defined from the public PE spec.
- `imports.hpp` / `imports.cpp` — import table reader. Lists every library the mapped image imports and, per
  function, its name or ordinal and the address-table slot the loader fills.
- `loader.hpp` / `loader.cpp` — image loader. Places the image at its preferred address (or elsewhere, applying
  its relocations), connects each import through a resolver, sets every section's page protection and registers
  the exception table. An import nothing provides gets a stub that names the function if it is called. The loader
  does not start the image and does not yet set up its thread-local storage.
- `host_libraries.hpp` / `host_libraries.cpp` — resolver that connects an import to the same-named library on
  the PC (or in a folder added to the search), and records which libraries do not load.
- `hosting.hpp` / `hosting.cpp` — flat hosting model. One contiguous GPU backing over the game's flat VA space;
  `device_address = heap_base + (game_va - flat_base)`; real pages bound on demand.
- `tools/` — command-line tools:
  - `list_imports <image file>` prints the imported libraries and function counts.
  - `describe_image <image file>` prints what the image asks of a loader: base, sections, data directories,
    relocations, thread-local storage.
  - `connect_imports <image file> [library folder] [--names]` loads the image without starting it and reports
    which imports connect and which are not provided.
- `tests/` — unit tests (no game data).

## Build + test

```
build.cmd
```

MSVC + the bundled CMake/Ninja. Builds `whiwhi_shell` and the tools, and runs `test_hosting`, `test_image`,
`test_imports` and `test_loader` via CTest. No GPU needed.
